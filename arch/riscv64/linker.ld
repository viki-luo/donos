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
                __bss_start = .;
        }

        .data : ALIGN(4K) {
                *(.data .data.* .sdata .sdata.*)
                *(.sbss .sbss.*)
                *(.bss .bss.*)
                *(COMMON)
                . = ALIGN(8);
                __data_end = .;
                __bss_end = .;
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
