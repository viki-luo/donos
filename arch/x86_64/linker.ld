/*
 * linker.ld — x86_64 内核链接脚本（PVH 引导）
 *
 * 三个要点：
 *
 *   1. PVH 入口 note 必须落在独立的 PT_NOTE 段里。
 *      QEMU 只从 PT_NOTE 读取引导信息，放进普通加载段它看不见。
 *
 *   2. 可写数据必须「文件承载」（FileSiz > 0）。
 *      QEMU 的 PVH 加载器只映射 ELF 段里 FileSiz 覆盖的那部分；
 *      纯 NOBITS 的 .bss 如果自成一个段（FileSiz = 0），那一段
 *      根本不会被映射，写它会直接页错误。所以这里把 .bss 并进
 *      .data，让整段都有文件字节。
 *
 *   3. 栈排在可写段的最后，且必须在 __bss_end 之后：
 *      启动代码要清零 .bss，不能把栈上的返回地址一起抹掉。
 *
 *   4. 入口是 32 位代码（pvh_entry32）。
 *      QEMU 按 XEN_ELFNOTE_PHYS32_ENTRY 进入时是 32 位保护模式，
 *      必须由 32 位 stub 完成 32→64 位切换。
 *
 *   5. 页表不放镜像里，由 32 位 stub 建在固定物理地址 0x70000
 *      （见 boot.asm），避免与 .bss 清零互相干扰。
 */
ENTRY(pvh_entry32)

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
                __text_start = .;
                *(.text .text.*)
                . = ALIGN(4K);
                __text_end = .;
                __rodata_start = .;
        } :text

        .rodata : ALIGN(4K) {
                *(.rodata .rodata.*)
                . = ALIGN(4K);
                __rodata_end = .;
                __data_start = .;
        } :rodata

        /*
         * 可写段：.data + .bss + 栈，全部文件承载。
         * 这样 FileSiz 覆盖整段，QEMU 一定会映射它。
         */
        .data : ALIGN(4K) {
                *(.data .data.*)

                . = ALIGN(16);
                __bss_start = .;
                *(COMMON)
                *(.bss .bss.*)
                . = ALIGN(16);
                __bss_end = .;

                /* 栈放在最后，向下增长，永远不会撞到上面的数据 */
                . = ALIGN(16);
                *(.bootstack)
                . = ALIGN(4K);
                __stack_top = .;
                __data_end = .;
        } :data

        /DISCARD/ : {
                *(.comment)
                *(.eh_frame)
                *(.note.gnu.property)
                *(.note.GNU-stack)
        }
}