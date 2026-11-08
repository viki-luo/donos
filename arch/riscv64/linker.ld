/*
 * linker.ld — riscv64（QEMU virt + OpenSBI）
 *
 * OpenSBI 的 fw_dynamic 在 S 态跳到 0x80200000，a0 = hartid，a1 = 设备树地址。
 * 栈放在 .bss 之后，与 x86_64 同一条规则：清 .bss 不会碰到栈。
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
        . = 0x80200000;

        .text : ALIGN(4K) {
                __text_start = .;
                *(.text.boot)
                *(.text .text.*)
                . = ALIGN(4K);

                /*
                 * .rodata 与栈之间必须各自对齐到 4 KiB：
                 * Sv39 的权限以 4 KiB 页为单位，跨页的段
                 * 会让「只读页」里混进可写内容（或反过来）。
                 */
                __text_end = .;
                __rodata_start = .;
        }

        .rodata : ALIGN(4K) {
                *(.rodata .rodata.* .srodata .srodata.*)
                . = ALIGN(4K);
                __rodata_end = .;
                __data_start = .;
        }

        .data : ALIGN(4K) {
                /*
                 * 初值区与清零区必须分开标记。
                 *
                 * 此前 `__bss_start` 定义在 `.data` 的最开头（甚至定义在
                 * .rodata 里），而 `.bss` 又并进了同一个输出段 —— boot.S
                 * 的清零循环于是把**整个 .data 的初值全部抹掉**。
                 * 任何带非零静态初值的全局变量在 riscv64 上都会读到 0，
                 * 而这类 bug 只在「第一次真的有非零初值」时才现形：
                 * 计时器节拍宽度被清零后，中断变成永不过期，
                 * 表现为节拍数暴涨到百万级、主循环被彻底饿死。
                 */
                *(.data .data.* .sdata .sdata.*)
                . = ALIGN(16);
                __data_init_end = .;

                __bss_start = .;
                *(.sbss .sbss.*)
                *(.bss .bss.*)
                *(COMMON)
                . = ALIGN(8);
                __bss_end = .;
                __data_end = .;
                __stack_bottom = .;
        }

        . = ALIGN(4K);
        . += 64 * 1024;
        __stack_top = .;


        /DISCARD/ : {
                *(.comment)
                *(.eh_frame)
                *(.riscv.attributes)
        }
}
