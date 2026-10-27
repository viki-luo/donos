/*
 * arch.h — x86_64 架构层接口
 *
 * 架构层只提供「机器能做什么」：端口 I/O、串口、中断、定时。
 * 它不认识物件，也不认识运动 —— 那是基座的事。
 *
 * 这条分工很重要：基座必须保持硬件无关，才能被搬到别的处理器架构上；
 * 一切和具体机器绑定的东西都收在本层。
 */
#ifndef LITEOS_ARCH_H
#define LITEOS_ARCH_H

#include "types.h"

/* ------------------------------------------------------------------ */
/* 端口 I/O                                                            */
/* ------------------------------------------------------------------ */

static inline void arch_outb(u16 port, u8 val)
{
        __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 arch_inb(u16 port)
{
        u8 ret;
        __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
        return ret;
}

static inline void arch_outl(u16 port, u32 val)
{
        __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline u32 arch_inl(u16 port)
{
        u32 ret;
        __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
        return ret;
}

/* ------------------------------------------------------------------ */
/* 控制台（COM1，已在引导阶段初始化）                                  */
/* ------------------------------------------------------------------ */

void arch_puts(const char *s);
void arch_putdec(u64 v);
void arch_puthex32(u32 v);
void arch_puthex64(u64 v);

/* ------------------------------------------------------------------ */
/* 中断                                                                */
/* ------------------------------------------------------------------ */

/* 建立 IDT，装好异常与 IRQ 的门。此时中断仍然关闭。 */
void arch_idt_init(void);

/* 重映射 8259 PIC 到 0x20/0x28，并只放开需要的线。 */
void arch_pic_init(void);

/* 启动 PIT 周期中断。hz 为每秒节拍数。 */
void arch_pit_init(u32 hz);

void arch_irq_enable(void);
void arch_irq_disable(void);

/* 自启动以来经过的节拍数。由 IRQ0 累加。 */
u64 arch_ticks(void);

#endif /* LITEOS_ARCH_H */
