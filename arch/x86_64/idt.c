/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * idt.c — IDT、8259 PIC、PIT 与异常派发
 *
 * 为什么先做这一层：在这之前，任何 CPU 异常都会变成三重故障，
 * 表现为「串口突然不出字了」，没有任何线索。有了 IDT，异常会明确
 * 打印向量号、错误码和出错指令地址 —— 这是后面一切调试的地基。
 *
 * 这一层属于架构，不属于基座。定时器是「机器的时间」，基座不持有时间：
 * 基座只负责把运动事件交给物件，此后物件按自己的节拍继续运动。
 */
#include "arch.h"

/* ------------------------------------------------------------------ */
/* IDT                                                                 */
/* ------------------------------------------------------------------ */

#define IDT_ENTRIES 256

typedef struct __attribute__((packed)) {
        u16 off_low;
        u16 selector;
        u8  ist;
        u8  type_attr;
        u16 off_mid;
        u32 off_high;
        u32 reserved;
} idt_entry_t;

typedef struct __attribute__((packed)) {
        u16 limit;
        u64 base;
} idtr_t;

static idt_entry_t idt[IDT_ENTRIES];
static idtr_t       idtr;

/* 由 isr.asm 提供：48 个向量存根地址，以及给未使用向量用的空存根 */
extern const u64 isr_stub_table[48];
extern void      isr_spurious(void);

/* 引导阶段的 64 位代码段选择子（见 boot.asm 的 GDT） */
#define KERNEL_CS 0x08

static void idt_set(u32 v, u64 handler)
{
        idt[v].off_low    = (u16)(handler & 0xFFFFu);
        idt[v].selector   = KERNEL_CS;
        idt[v].ist        = 0;
        idt[v].type_attr  = 0x8E;   /* present | DPL=0 | 64 位中断门 */
        idt[v].off_mid    = (u16)((handler >> 16) & 0xFFFFu);
        idt[v].off_high   = (u32)(handler >> 32);
        idt[v].reserved   = 0;
}

void arch_idt_init(void)
{
        for (u32 v = 0; v < IDT_ENTRIES; v++)
                idt_set(v, (u64)(uintptr_t)&isr_spurious);

        for (u32 v = 0; v < 48; v++)
                idt_set(v, isr_stub_table[v]);

        idtr.limit = (u16)(sizeof(idt) - 1);
        idtr.base  = (u64)(uintptr_t)&idt[0];

        __asm__ volatile("lidt %0" : : "m"(idtr));
}

/* ------------------------------------------------------------------ */
/* 8259 PIC                                                            */
/* ------------------------------------------------------------------ */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI   0x20

static void io_wait(void)
{
        /* 往未使用的端口写一次，给老式 PIC 一点时间 */
        arch_outb(0x80, 0);
}

void arch_pic_init(void)
{
        /* 重新编程：把 IRQ0..15 挪到 0x20..0x2F，避开 CPU 异常向量 */
        arch_outb(PIC1_CMD, 0x11); io_wait();
        arch_outb(PIC2_CMD, 0x11); io_wait();
        arch_outb(PIC1_DATA, 0x20); io_wait();   /* 主片偏移 0x20 */
        arch_outb(PIC2_DATA, 0x28); io_wait();   /* 从片偏移 0x28 */
        arch_outb(PIC1_DATA, 0x04); io_wait();   /* 从片挂在 IRQ2 */
        arch_outb(PIC2_DATA, 0x02); io_wait();
        arch_outb(PIC1_DATA, 0x01); io_wait();   /* 8086 模式 */
        arch_outb(PIC2_DATA, 0x01); io_wait();

        /*
         * 只放开 IRQ0（PIT）。其余全部屏蔽：还没有对应的驱动，
         * 放开只会收到无人处理的中断。
         */
        arch_outb(PIC1_DATA, 0xFE);
        arch_outb(PIC2_DATA, 0xFF);
}

/* ------------------------------------------------------------------ */
/* PIT                                                                 */
/* ------------------------------------------------------------------ */

#define PIT_CH0  0x40
#define PIT_CMD  0x43
#define PIT_BASE 1193182u

static volatile u64 tick_count;

void arch_pit_init(u32 hz)
{
        if (hz == 0)
                hz = 1;

        u32 divisor = PIT_BASE / hz;
        if (divisor == 0)
                divisor = 1;

        arch_outb(PIT_CMD, 0x36);                        /* 通道0，低高字节，模式3 */
        arch_outb(PIT_CH0, (u8)(divisor & 0xFFu));
        arch_outb(PIT_CH0, (u8)((divisor >> 8) & 0xFFu));
}

u64 arch_ticks(void)
{
        return tick_count;
}

/* 中断风暴测试：直接重编 PIT；额定频率恢复也走同一条路。 */
void arch_timer_hz(u32 hz)
{
        arch_pit_init(hz);
}

void arch_irq_enable(void)
{
        __asm__ volatile("sti");
}

void arch_irq_disable(void)
{
        __asm__ volatile("cli");
}

/* ------------------------------------------------------------------ */
/* 派发                                                                */
/*                                                                    */
/* 由 isr.asm 的公共流程调用。异常一律停机并打印现场 —— 裸机下没有  */
/* 「恢复」可言，把现场留下比继续跑更有价值。                          */
/* ------------------------------------------------------------------ */

typedef struct {
        volatile bool armed;
        volatile bool seen;
        u64 error;
        u64 rip;
        u64 cr2;
        u64 resume;
        u64 observed_error;
        u64 observed_rip;
        u64 observed_cr2;
        u32 hits;
} expected_fault_t;

static expected_fault_t expected_fault;

void arch_fault_expect(u64 error, u64 rip, u64 cr2, u64 resume)
{
        expected_fault.armed = false;
        expected_fault.seen = false;
        expected_fault.error = error;
        expected_fault.rip = rip;
        expected_fault.cr2 = cr2;
        expected_fault.resume = resume;
        expected_fault.hits = 0;
        expected_fault.observed_error = 0;
        expected_fault.observed_rip = 0;
        expected_fault.observed_cr2 = 0;
        __asm__ volatile("" ::: "memory");
        expected_fault.armed = true;
}

bool arch_fault_result(u64 *error, u64 *rip, u64 *cr2)
{
        if (error != 0)
                *error = expected_fault.observed_error;
        if (rip != 0)
                *rip = expected_fault.observed_rip;
        if (cr2 != 0)
                *cr2 = expected_fault.observed_cr2;
        return expected_fault.seen && !expected_fault.armed;
}

void isr_dispatch(u64 vector, u64 error, u64 *saved_rip, u64 cr2)
{
        if (vector == 14 && expected_fault.armed) {
                expected_fault.observed_error = error;
                expected_fault.observed_rip = *saved_rip;
                expected_fault.observed_cr2 = cr2;

                if (error == expected_fault.error &&
                    *saved_rip == expected_fault.rip &&
                    cr2 == expected_fault.cr2) {
                        expected_fault.armed = false;
                        expected_fault.seen = true;
                        *saved_rip = expected_fault.resume;
                        return;
                }

                expected_fault.armed = false;
                arch_puts("\n!! expected #PF mismatch\n");
        }

        if (vector < 32) {
                arch_puts("\n!! CPU exception\n");
                arch_puts("   vector = ");
                arch_putdec(vector);
                arch_puts("\n   error  = 0x");
                arch_puthex32((u32)error);
                arch_puts("\n   rip    = 0x");
                arch_puthex64(*saved_rip);
                if (vector == 14) {
                        arch_puts("\n   cr2    = 0x");
                        arch_puthex64(cr2);
                }
                arch_puts("\n");

                for (;;)
                        __asm__ volatile("cli; hlt");
        }

        if (vector >= 32 && vector < 48) {
                u32 irq = (u32)(vector - 32);

                if (irq == 0)
                        tick_count++;

                if (irq >= 8)
                        arch_outb(PIC2_CMD, PIC_EOI);
                arch_outb(PIC1_CMD, PIC_EOI);
        }
}
