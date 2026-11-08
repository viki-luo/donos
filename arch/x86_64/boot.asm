; SPDX-License-Identifier: GPL-2.0-only
; boot.asm — x86_64 引导入口（PVH 模式）
;
; 关键事实：XEN_ELFNOTE_PHYS32_ENTRY 里的 PHYS32 就是「32 位入口」。
; QEMU 按这个 note 跳进来时，CPU 处于 32 位保护模式，不是长模式。
;
; 所以入口必须是 32 位代码，由它完成标准的 32→64 位切换：
;   CR4.PAE → CR3 → EFER.LME → CR0.PG → 加载 64 位 GDT → 远跳转
;
; 如果直接按 64 位写，REX 前缀（0x48）会被当成 dec eax，
; lea 会退化成「把位移当结果」，整段代码都在被错误解码。
;
; 分步标记（便于定位）：
;   'A' 串口就绪（32 位）
;   'B' 已进入 64 位、页表已生效
;   'C' 即将进入 C

%define COM1      0x3F8
%define PML4_PHYS 0x70000
%define PDPT_PHYS 0x71000
%define PD_PHYS   0x72000
%define BOOTINFO  0x74000

; 默认使用绝对寻址。栈与 .bss 边界那几处显式写成 [rel ...]，
; 以免把符号的链接地址误当成数据。
DEFAULT ABS

; 引导阶段单字母标记（A/B/C/D）。默认关闭；make BOOT_TRACE=1 打开，
; 用于定位卡在哪个阶段。注意 32 位阶段用 putc32，64 位阶段用 putc。

; ---------------------------------------------------------------------
; PVH 入口 note —— 必须落在 PT_NOTE 段
; desc 里放的是 32 位入口的物理地址
; ---------------------------------------------------------------------
section .note.xen note alloc
align 4
        dd 4                    ; namesz
        dd 4                    ; descsz
        dd 18                   ; XEN_ELFNOTE_PHYS32_ENTRY
        db "Xen", 0
align 4
        dd pvh_entry32          ; 32 位入口
align 4

; ---------------------------------------------------------------------
; 栈（文件承载，见链接脚本说明）
; ---------------------------------------------------------------------
section .bootstack alloc noexec write
align 4096
stack_bottom:
        ; 用显式零字节而不是 resb：这里是文件承载段，栈必须在文件里有
        ; 实际字节，QEMU 的 PVH 加载器才会为它建立映射。
        times 64 * 1024 db 0
stack_top:

; ---------------------------------------------------------------------
; 64 位 GDT（必须可写段承载，切换后要用）
; ---------------------------------------------------------------------
section .data
align 8
gdt64:
        dq 0x0000000000000000           ; null
        dq 0x00AF9A000000FFFF           ; 0x08：64 位代码，L=1
        dq 0x00CF92000000FFFF           ; 0x10：数据
gdt64_end:
gdtr:
        dw gdt64_end - gdt64 - 1
        dd gdt64

; ---------------------------------------------------------------------
section .text
global pvh_entry32
extern kernel_main
extern __bss_start
extern __bss_end

; ===================== 32 位阶段 =====================
BITS 32

; 串口输出 al（32 位版）
putc32:
        push    eax
        push    edx
.wait:
        mov     dx, COM1 + 5
        in      al, dx
        test    al, 0x20
        jz      .wait
        pop     edx
        pop     eax
        push    edx
        mov     dx, COM1
        out     dx, al
        pop     edx
        ret

pvh_entry32:
        cli
        cld

        ; PVH 在 ebx 里给 start_info 指针；先存起来，64 位阶段要用
        mov     [BOOTINFO], ebx

        ; 32 位阶段用一个低地址临时栈
        mov     esp, 0x80000

        ; ---- 串口初始化（115200 8N1）----
        xor     eax, eax
        mov     dx, COM1 + 1
        out     dx, al                  ; 关中断

        mov     al, 0x80
        mov     dx, COM1 + 3
        out     dx, al                  ; DLAB

        mov     al, 0x01
        mov     dx, COM1 + 0
        out     dx, al                  ; 除数低字节
        xor     eax, eax
        mov     dx, COM1 + 1
        out     dx, al                  ; 除数高字节

        mov     al, 0x03
        mov     dx, COM1 + 3
        out     dx, al                  ; 8N1

        mov     al, 0xC7
        mov     dx, COM1 + 2
        out     dx, al                  ; FIFO

        mov     al, 0x0B
        mov     dx, COM1 + 4
        out     dx, al                  ; DTR/RTS/OUT2

%ifdef BOOT_TRACE
        mov     al, 'A'
        call    putc32
%endif

        ; ---- 建立页表（恒等映射前 1 GiB，2 MiB 大页）----
        ; 先清零三张表
        mov     edi, PML4_PHYS
        xor     eax, eax
        mov     ecx, 3 * 512 * 2        ; 3 表 × 512 项 × 2 个 dword
        rep     stosd

        ; PML4[0] -> PDPT
        mov     edi, PML4_PHYS
        mov     eax, PDPT_PHYS + 3
        mov     [edi], eax

        ; PDPT[0] -> PD
        mov     edi, PDPT_PHYS
        mov     eax, PD_PHYS + 3
        mov     [edi], eax

        ; PD 填 512 个 2 MiB 大页
        mov     edi, PD_PHYS
        mov     eax, 0x83               ; present | writable | PS
        mov     ecx, 512
.fill:
        mov     [edi], eax
        add     eax, 0x200000
        add     edi, 8
        loop    .fill

        ; ---- 开启 PAE ----
        mov     eax, cr4
        or      eax, 0x20               ; CR4.PAE
        mov     cr4, eax

        ; ---- 装载 CR3 ----
        mov     eax, PML4_PHYS
        mov     cr3, eax

        ; ---- 开启长模式（EFER.LME）----
        mov     ecx, 0xC0000080
        rdmsr
        or      eax, 0x100              ; EFER.LME
        wrmsr

        ; ---- 开启分页 ----
        mov     eax, cr0
        or      eax, 0x80000000         ; CR0.PG
        mov     cr0, eax

        ; ---- 加载 64 位 GDT 并远跳转 ----
        lgdt    [gdtr]
        jmp     0x08:entry64

; ===================== 64 位阶段 =====================
BITS 64

; 串口输出 al（64 位版）
putc:
        push    rax
        push    rdx
.wait:
        mov     dx, COM1 + 5
        in      al, dx
        test    al, 0x20
        jz      .wait
        pop     rdx
        pop     rax
        push    rdx
        mov     dx, COM1
        out     dx, al
        pop     rdx
        ret

entry64:
        ; 切到真正的 64 位栈
        lea     rsp, [rel stack_top]
        xor     rbp, rbp

%ifdef BOOT_TRACE
        mov     al, 'B'
        call    putc
%endif

        ; ---- 清零 .bss ----
        ; 页表现已在固定的低地址，不在 .bss 里，清零不会破坏它。
        lea     rdi, [rel __bss_start]
        lea     rcx, [rel __bss_end]
        sub     rcx, rdi
        jbe     .bss_done
        shr     rcx, 3
        xor     eax, eax
        rep     stosq
.bss_done:

%ifdef BOOT_TRACE
        mov     al, 'C'
        call    putc
%endif

        ; ---- 进 C ----
        mov     edi, [BOOTINFO]
        call    kernel_main

%ifdef BOOT_TRACE
        mov     al, 'D'
        call    putc
%endif

.hang:
        hlt
        jmp     .hang

section .note.GNU-stack noalloc noexec nowrite progbits