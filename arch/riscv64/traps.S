/*
 * traps.S — riscv64 陷阱入口
 *
 * stvec 用直接模式（低两位为 0），因此所有陷阱都跳到 trap_entry。
 * 这里保存全部被中断代码可能用到的寄存器 —— 中断是异步发生的，
 * 不能假设对方只用到调用者保存寄存器。
 *
 * 栈上布局：x1 与 x3..x31 各 8 字节，共 240 字节，留 256 保持 16 字节对齐。
 */
        .section .text
        .p2align 2
        .globl  trap_entry
trap_entry:
        addi    sp, sp, -256

        sd      x1,    0(sp)
        sd      x3,    8(sp)
        sd      x4,   16(sp)
        sd      x5,   24(sp)
        sd      x6,   32(sp)
        sd      x7,   40(sp)
        sd      x8,   48(sp)
        sd      x9,   56(sp)
        sd      x10,  64(sp)
        sd      x11,  72(sp)
        sd      x12,  80(sp)
        sd      x13,  88(sp)
        sd      x14,  96(sp)
        sd      x15, 104(sp)
        sd      x16, 112(sp)
        sd      x17, 120(sp)
        sd      x18, 128(sp)
        sd      x19, 136(sp)
        sd      x20, 144(sp)
        sd      x21, 152(sp)
        sd      x22, 160(sp)
        sd      x23, 168(sp)
        sd      x24, 176(sp)
        sd      x25, 184(sp)
        sd      x26, 192(sp)
        sd      x27, 200(sp)
        sd      x28, 208(sp)
        sd      x29, 216(sp)
        sd      x30, 224(sp)
        sd      x31, 232(sp)

        csrr    a0, scause
        csrr    a1, sepc
        csrr    a2, stval
        call    trap_dispatch

        ld      x1,    0(sp)
        ld      x3,    8(sp)
        ld      x4,   16(sp)
        ld      x5,   24(sp)
        ld      x6,   32(sp)
        ld      x7,   40(sp)
        ld      x8,   48(sp)
        ld      x9,   56(sp)
        ld      x10,  64(sp)
        ld      x11,  72(sp)
        ld      x12,  80(sp)
        ld      x13,  88(sp)
        ld      x14,  96(sp)
        ld      x15, 104(sp)
        ld      x16, 112(sp)
        ld      x17, 120(sp)
        ld      x18, 128(sp)
        ld      x19, 136(sp)
        ld      x20, 144(sp)
        ld      x21, 152(sp)
        ld      x22, 160(sp)
        ld      x23, 168(sp)
        ld      x24, 176(sp)
        ld      x25, 184(sp)
        ld      x26, 192(sp)
        ld      x27, 200(sp)
        ld      x28, 208(sp)
        ld      x29, 216(sp)
        ld      x30, 224(sp)
        ld      x31, 232(sp)

        addi    sp, sp, 256
        sret
