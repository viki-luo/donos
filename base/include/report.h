/*
 * report.h — 诊断输出接口
 *
 * 只有名字带 arch_ 的实现才和具体机器绑定；这里声明的是一个抽象的
 * 「报告」能力。基座本身不打印任何东西，只有自检需要它。
 *
 * 这样 base/ 下的测试代码不必去包含 x86 的头文件，分层不会被测试破坏。
 */
#ifndef LITEOS_REPORT_H
#define LITEOS_REPORT_H

#include "types.h"

void arch_puts(const char *s);
void arch_putdec(u64 v);
void arch_puthex32(u32 v);
void arch_puthex64(u64 v);

/*
 * 单调递增的周期计数，只用于测量。
 *
 * 它不参与控制逻辑：基座既不计时也不调度（设计决定 16）。这里引入它
 * 是为了让性能数字可以在目标机上复核，而不是靠推算。
 *
 * 注意它和 arch_ticks() 来自同一条时间基准，但刻度不同 —— 因此
 * 「一次推进花掉一个节拍的百分之几」这种比值是有意义的，而
 * 「每次调用多少纳秒」在模拟器下不具绝对意义。
 */
u64 arch_cycles(void);

/* 自开机以来的定时器节拍数。频率由架构层决定（两架构均为 100 Hz）。 */
u64 arch_ticks(void);

#endif /* LITEOS_REPORT_H */
