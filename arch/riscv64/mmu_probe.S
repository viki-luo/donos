/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mmu_probe.S - recoverable Sv39 fault probes
 *
 * The trap dispatcher replaces sepc with the exported resume label after it
 * verifies the exact expected cause, fault PC and stval. No instruction length
 * is guessed, which also keeps this correct with compressed instructions.
 */
        .section .text
        .p2align 2

        .globl arch_probe_store_ro
        .globl arch_probe_store_fault
        .globl arch_probe_store_resume
arch_probe_store_ro:
arch_probe_store_fault:
        sd      zero, 0(a0)
arch_probe_store_resume:
        ret

        .globl arch_probe_exec_nx
        .globl arch_probe_exec_resume
arch_probe_exec_nx:
        jalr    zero, 0(a0)
arch_probe_exec_resume:
        ret

        .section .note.GNU-stack,"",@progbits
