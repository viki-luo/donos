/*
 * linker.ld — x86_64 内核链接脚本（PVH 引导）
 *
 * 两个要点：
 *   1. PVH 入口 note 必须落在独立的 PT_NOTE 段里。
 *      QEMU 只从 PT_NOTE 读取引导信息，放进普通加载段它看不见。
 *   2. 内核加载到 1 MiB，与启动代码里建立的恒等映射一致。
 */
ENTRY(pvh_entry)

PHDRS
{
        note   PT_NOTE FLAGS(4);
        text   PT_LOAD FLAGS(5);   /* R + X */
        rodata PT_LOAD FLAGS(4);   /* R     */
        data   PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
        . = 1M;

        .note.xen : ALIGN(4) {
                KEEP(*(.note.xen))
        } :note

        .text : ALIGN(4K) {
                *(.text .text.*)
        } :text

        .rodata : ALIGN(4K) {
                *(.rodata .rodata.*)
        } :rodata

        .data : ALIGN(4K) {
                *(.data .data.*)
        } :data

        .bss : ALIGN(4K) {
                *(COMMON)
                *(.bss .bss.*)
        } :data

        /DISCARD/ : {
                *(.comment)
                *(.eh_frame)
                *(.note.gnu.property)
                *(.note.GNU-stack)
        }
}