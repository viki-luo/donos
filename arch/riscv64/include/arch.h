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

#endif /* LITEOS_ARCH_H */
