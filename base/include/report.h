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

#endif /* LITEOS_REPORT_H */
