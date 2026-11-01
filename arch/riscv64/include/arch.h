/*
 * arch.h — riscv64 架构层接口
 *
 * 与 x86_64 的 arch.h 对外提供同一组控制台函数；基座只依赖
 * report.h 里声明的这几个名字，不知道自己跑在哪种处理器上。
 */
#ifndef LITEOS_ARCH_H
#define LITEOS_ARCH_H

#include "types.h"

/* 控制台（QEMU virt 的 16550 UART，0x10000000） */
void arch_puts(const char *s);
void arch_putdec(u64 v);
void arch_puthex32(u32 v);
void arch_puthex64(u64 v);

/* QEMU virt 的 time CSR 频率为 10 MHz */
#define ARCH_TIME_HZ 10000000ULL

static inline u64 arch_time(void)
{
        u64 t;
        __asm__ volatile("rdtime %0" : "=r"(t));
        return t;
}

static inline u64 arch_time_ns(void)
{
        return arch_time() * (1000000000ULL / ARCH_TIME_HZ);
}

/* ------------------------------------------------------------------ */
/* 定时器中断                                                          */
/* ------------------------------------------------------------------ */

/* 每个节拍对应的 time 计数周期数：10 MHz / 100 Hz = 100000 */
#define ARCH_TICK_CYCLES 100000ULL

/* 建立陷阱向量，编程 CLINT 定时器，并在 sstatus 中打开 S 态中断。 */
void arch_timer_init(u64 hartid);

void arch_irq_enable(void);
void arch_irq_disable(void);

/* 自开机以来经过的节拍数，由定时器中断累加。 */
u64 arch_ticks(void);

/* 每节拍对应的纳秒数 */
#define ARCH_NS_PER_TICK (1000000000ULL / (ARCH_TIME_HZ / ARCH_TICK_CYCLES))

/* ------------------------------------------------------------------ */
/* 测量                                                                */
/* ------------------------------------------------------------------ */

/*
 * 测量用周期计数：直接取 time 计数（10 MHz）。
 *
 * 不用 rdcycle：S 态能否读它取决于 mcounteren 的 CY 位，而 time 位
 * 已经确认放开。刻度粗一些（100 ns），所以只测「成批调用」的总量，
 * 不测单次调用。
 */
u64 arch_cycles(void);

/* ------------------------------------------------------------------ */
/* 保护状态的自我报告                                                  */
/* ------------------------------------------------------------------ */

/*
 * 打印当前地址翻译状态（satp）。
 *
 * satp = 0 表示 Bare 模式：不做地址翻译，也没有任何权限位可查。
 * 在 S 态下这意味着所有内存对内核都是可读可写可执行的，而且这里
 * 没有用户态 —— 上层大脑与内核同权。
 *
 * 这个事实由机器自己打印，而不是由文档声称。
 */
void arch_report_protection(void);

#endif /* LITEOS_ARCH_H */
