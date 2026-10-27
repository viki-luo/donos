# 引导路径 / Boot Path

[English](boot.en.md) | 简体中文

本文记录 x86_64 上的引导细节。这些内容不是设计哲学（见
[design.md](design.md)），而是**实现约束**——写错任何一条，内核都会在
串口上只吐出一个字符然后停住。

---

## 1. 为什么用 PVH

QEMU 的 `-kernel` 支持几种内核格式。实测结论：

| 格式 | 结果 |
| --- | --- |
| 64 位 multiboot 1 | `Cannot load x86-64 image, give a 32bit one` |
| multiboot 2 | 需要额外的 32 位头与对齐，且 QEMU 支持有限 |
| **PVH（Xen ELF note）** | 可用 |

PVH 的做法是在 ELF 里放一个 note，告诉加载器「从这个地址开始执行」：

```asm
section .note.xen note alloc
        dd 4                    ; namesz
        dd 4                    ; descsz
        dd 18                   ; XEN_ELFNOTE_PHYS32_ENTRY
        db "Xen", 0
        dd pvh_entry32          ; 入口地址
```

这个 note **必须落在独立的 `PT_NOTE` 段**里。放进普通加载段，
QEMU 看不见。

---

## 2. `PHYS32` 是「32 位入口」

这是最容易踩的坑，值得单独强调。

`XEN_ELFNOTE_PHYS32_ENTRY` 里的 **32 不是笔误**：按这个 note 进入时，
CPU 处于 **32 位保护模式**，不是长模式。

如果入口直接按 64 位写，会发生什么？

x86_64 的 `REX` 前缀是 `0x40`–`0x4F`。在 32 位模式下，这些字节是
单字节指令 `inc`/`dec`。于是：

```
48 8d 05 fe 2e 00 00   64 位解码：lea rax, [rip+0x2efe]   → rax = 0x104000
                       32 位解码：dec eax
                                  lea eax, [0x2efe]        → eax = 0x2efe
```

结果就是 `rax` 等于**位移量本身**，而不是算出来的地址。症状是
`lea rsp, [rel stack_top]` 之后 `rsp` 变成一个小数字，`mov cr3` 之后
`cr3` 变成位移量。代码不会立刻崩，因为它每一步都在被错误解码，
只是行为完全不对——这种 bug 极难从现象反推。

**结论：PVH 入口必须是 32 位 stub，由它完成 32→64 位切换。**

---

## 3. 32→64 位切换顺序

顺序不能乱：

```
1. CR4.PAE  = 1            开启物理地址扩展（长模式的先决条件）
2. CR3      = PML4 物理地址
3. EFER.LME = 1            通过 WRMSR 写 IA32_EFER
4. CR0.PG   = 1            开启分页 → 进入长模式兼容状态（仍是 32 位代码）
5. LGDT     + 远跳转到 L=1 的代码段 → 真正进入 64 位模式
```

注意第 4 步之后、第 5 步之前，CPU 处于「长模式兼容状态」：已经在
长模式，但仍按 32 位方式解码。此时必须用远跳转（`jmp 0x08:entry64`）
配合一个 `L=1` 的代码段描述符，才会真正切到 64 位。

GDT 里的 64 位代码段描述符：

```
dq 0x00AF9A000000FFFF       ; P=1 DPL=0 S=1 type=code, G=1 L=1 D=0
```

---

## 4. 页表放在固定低地址

页表建在固定的物理地址，而不是内核镜像的 `.bss` 里：

```
PML4  0x70000
PDPT  0x71000
PD    0x72000
```

原因有两个：

1. **恒等映射**。前 1 GiB 用 2 MiB 大页恒等映射，虚拟地址 = 物理地址。
   stub 在 32 位阶段用物理地址直接写这几张表，切到 64 位后照样能访问。
2. **与 `.bss` 清零解耦**。启动代码要清零 `.bss`；如果页表和 `.bss`
   在同一块内存里，清零顺序稍有差错就会把页表抹掉，表现为「分页
   莫名失效」。

---

## 5. 可写段必须「文件承载」

QEMU 的 PVH 加载器按 ELF 段的 **`FileSiz`** 建立映射。纯 `NOBITS`
的 `.bss` 如果自成一个段，`FileSiz = 0`——**那一段根本不会被映射**，
写它会直接页错误（而且因为此时还没有 IDT，页错误会变成三重故障，
表现为「静默停住」）。

所以链接脚本把 `.bss` 并进文件承载的 `.data` 段：

```ld
.data : ALIGN(4K) {
        *(.data .data.*)
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
        *(.bootstack)
} :data
```

同理，栈用 `times 64 * 1024 db 0` 而不是 `resb`，保证它在文件里有
实际字节。

验证方法：

```sh
readelf -l liteos.bin | grep -A2 LOAD
# 可写段的 FileSiz 必须非零
```

---

## 6. 栈的位置

栈排在可写段的**最后**，并且在 `__bss_end` 之后。两个理由：

- 栈向下增长，放在最末尾就不会撞进任何数据。
- 启动代码清零 `.bss` 时，不会连带把栈上的返回地址一起抹掉。

早期版本的 bug 就是栈顶正好压在接线表起点上：`base_init()` 的返回
地址刚压栈就被清零，`ret` 跳进垃圾地址。

---

## 7. 排错手段

### 引导阶段标记

默认关闭。打开后每个阶段吐一个字母：

```sh
make BOOT_TRACE=1
```

```
'A' 串口就绪（32 位阶段）
'B' 已进入 64 位、页表已生效
'C' 即将进入 C
'D' kernel_main 返回
```

看到 `A` 之后没有 `B`，问题在页表或 32→64 切换；看到 `C` 但没有
后续输出，问题在 C 代码。

### 用 gdb 看真实寄存器

```sh
qemu-system-x86_64 -kernel liteos.bin -m 256M \
        -serial file:build/serial.log -display none \
        -no-reboot -no-shutdown -gdb tcp::1240 -S
gdb -q liteos.bin -ex 'target remote localhost:1240'
```

**注意**：每次调试前先清掉残留的 QEMU 进程。否则新 QEMU 绑定不了
端口，gdb 会连到**上一个实例**（跑的是旧内核），看到的寄存器与
反汇编对不上，会把人带进沟里。

### 判断分页是否真的生效

只看 `mov cr3` 执行了没有是不够的。在 C 里读回控制寄存器：

```c
u64 cr0, cr4, cr3;
__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
```

正确状态（前 1 GiB 恒等映射）：

```
cr0 = 0x80000011     ; PG=1 PE=1 ET=1
cr4 = 0x00000020     ; PAE=1
cr3 = 0x00070000     ; 我们的 PML4
```

如果 `cr3` 不是 `0x70000`，说明 `mov cr3` 没有按预期执行——先怀疑
是不是根本还在 32 位模式（见第 2 节）。
