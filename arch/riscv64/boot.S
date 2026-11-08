/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * boot.S — riscv64 入口（QEMU virt + OpenSBI，S 态）
 *
 * OpenSBI 进入时：a0 = hartid，a1 = 设备树地址，分页关闭。
 * 只做三件事：设栈、清 .bss、进 C。
 */
        .section .text.boot
        .globl  _start
_start:
        la      sp, __stack_top
        mv      s0, a0
        mv      s1, a1

        la      t0, __bss_start
        la      t1, __bss_end
1:      bgeu    t0, t1, 2f
        sd      zero, 0(t0)
        addi    t0, t0, 8
        j       1b
2:
        mv      a0, s0
        mv      a1, s1
        call    kernel_main

3:      wfi
        j       3b
