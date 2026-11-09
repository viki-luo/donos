/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arch.h — riscv64 架构层接口
 *
 * 与 x86_64 的 arch.h 对外提供同一组控制台函数；基座只依赖
 * report.h 里声明的这几个名字，不知道自己跑在哪种处理器上。
 */
#ifndef DONOS_ARCH_H
#define DONOS_ARCH_H

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

/* 只安装 S 态陷阱向量，不打开中断；MMU 故障注入在定时器之前需要它。 */
void arch_trap_init(void);

/* 建立陷阱向量，编程定时器，并在 sstatus 中打开 S 态中断。 */
void arch_timer_init(u64 hartid);

void arch_irq_enable(void);
void arch_irq_disable(void);

/* 自开机以来经过的节拍数，由定时器中断累加。 */
u64 arch_ticks(void);

/* 每节拍对应的纳秒数 */
#define ARCH_NS_PER_TICK (1000000000ULL / (ARCH_TIME_HZ / ARCH_TICK_CYCLES))

/* 临时把节拍中断改到 hz；额定值见 report.h 的 ARCH_TICK_HZ。 */
void arch_timer_hz(u32 hz);

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

/* ------------------------------------------------------------------ */
/* MMU 隔离（Sv39，只打开内核自己需要的那部分映射）                     */
/* ------------------------------------------------------------------ */

/*
 * 建立 Sv39 页表并切换到它。映射范围是 0..4 MiB 的恒等映射：
 * 代码 R|X、只读数据 R、其余 .data/.bss/栈/页表 R|W。
 * UART 与 PCIe ECAM 各映射一个 2 MiB 大页；PMP 放行并不能替代分页，
 * 因为 PMP 与页表是两道独立检查。
 *
 * 返回 0 表示成功；负值表示失败，此时仍停在 Bare 模式上，机器可用。
 */
int arch_mmu_init(void);

/* 读 satp 当前值（用于自检打印与判定）。 */
u64 arch_satp(void);

/* 走一遍页表，把某一页的权限位读回来（page 是从 KERNEL_BASE 起算的页号）。 */
u64  arch_mmu_walk(u64 page);

/* 同上，一次取代码 / 只读 / 数据三个探针页。 */
void arch_mmu_walk3(u64 out[3]);

/* Sv39 权限位，供自检比对。 */
#define PTE_R 0x002ULL
#define PTE_W 0x004ULL
#define PTE_X 0x008ULL

/* 可恢复的预期故障：仅供 MMU 自检使用。 */
void arch_fault_expect(u64 cause, u64 sepc, u64 stval, u64 resume);
bool arch_fault_result(u64 *cause, u64 *sepc, u64 *stval);

/* 固定汇编探针和符号，避免按指令长度猜测恢复地址。 */
void arch_probe_store_ro(void *target);
void arch_probe_exec_nx(void *target);
extern char arch_probe_store_fault[];
extern char arch_probe_store_resume[];
extern char arch_probe_exec_resume[];

/* 打印并核对当前保护状态；返回失败计数。 */
int arch_mmu_verify(void);

#endif /* DONOS_ARCH_H */
