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

        .text : {
                *(.text.boot)
                *(.text .text.*)
        }

        .rodata : ALIGN(16) {
                *(.rodata .rodata.* .srodata .srodata.*)
        }

        .data : ALIGN(16) {
                *(.data .data.* .sdata .sdata.*)
        }

        .bss : ALIGN(16) {
                __bss_start = .;
                *(.sbss .sbss.*)
                *(.bss .bss.*)
                *(COMMON)
                . = ALIGN(8);
                __bss_end = .;
        }

        . = ALIGN(16);
        . += 64 * 1024;
        __stack_top = .;

        /DISCARD/ : {
                *(.comment)
                *(.eh_frame)
                *(.riscv.attributes)
        }
}
