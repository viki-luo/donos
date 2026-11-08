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

/* 临时把节拍中断改到 hz；额定值见 report.h 的 ARCH_TICK_HZ。 */
void arch_timer_hz(u32 hz);

/* ------------------------------------------------------------------ */
/* 测量                                                                */
/* ------------------------------------------------------------------ */

/*
 * 时间戳计数器。只用于测量，不参与控制逻辑。
 *
 * 在虚拟机里它跟随宿主时钟，所以「每次调用多少周期」只在同一次运行内
 * 可比；比值类指标（一次推进占一个节拍的比例）不受刻度影响。
 */
u64 arch_cycles(void);

/* ------------------------------------------------------------------ */
/* 保护状态的自我报告                                                  */
/* ------------------------------------------------------------------ */

/*
 * 打印当前分页与保护状态（CR0 / CR4 / CR3 与页表项）。
 *
 * 存在的理由：说「有分页」和说「分页提供了保护」是两件不同的事。
 * 引导阶段建的是恒等映射，页目录项是 0x83 —— 存在、可写、PS，
 * 既没有 NX 位（bit 63），也没有用户位（bit 2），CR0.WP 也没置。
 * 也就是说：全部内存可读可写可执行，内核态与用户态没有分离。
 * 这个事实必须由机器自己打印出来，而不是由文档声称。
 */
void arch_report_protection(void);

/* ------------------------------------------------------------------ */
/* MMU 隔离（W^X 中能真正装上的那一部分）                               */
/* ------------------------------------------------------------------ */

/*
 * 打开 EFER.NXE。CPU 不支持时返回 false —— 调用者必须据此决定是否
 * 报告「支持不可执行页」，而不是无条件声称已经支持。
 */
bool arch_mmu_enable_nx(void);

/* 把 2 MiB 及以上的大页标为不可执行，返回实际标记的页数。 */
u32 arch_mmu_mark_data_nx(void);

/* 打开 CR0.WP：内核态写也必须遵守页表项的可写位。 */
bool arch_mmu_enable_wp(void);

/* 读某一 2 MiB 页的页目录项。 */
u64 arch_mmu_entry(u32 page_2mib);

/* 改某一 2 MiB 页的可写位，返回该项原值（用于复原）。 */
u64 arch_mmu_set_writable(u32 page_2mib, bool writable);

/*
 * 强行拨动 CR0.WP，用于证明「权限位 + 保护位」缺一不可：
 * 项标了只读而 WP=0 时，内核态照样写得进去。
 */
void arch_mmu_set_wp(bool on);

/* 精确恢复一个 2 MiB 页目录项并刷新地址翻译。 */
void arch_mmu_set_entry(u32 page_2mib, u64 entry);

/*
 * 把第 0 个 2 MiB 大页拆成 4 KiB 页，实现完整 W^X。
 * 必须先 arch_mmu_enable_nx()。返回实际建立的页数。
 */
u32 arch_mmu_split_kernel(void);

/* 读第 0 个 4 KiB 页表项（page_4kib 为 0..511，自检用）。 */
u64 arch_mmu_pte(u32 page_4kib);

/* 可恢复的预期 #PF，仅用于 MMU 自检。 */
void arch_fault_expect(u64 error, u64 rip, u64 cr2, u64 resume);
bool arch_fault_result(u64 *error, u64 *rip, u64 *cr2);

/* 固定汇编探针；恢复地址是导出标签，不靠猜测指令长度。 */
void arch_probe_write(void *target);
void arch_probe_exec(void *target);
extern char arch_probe_write_fault[];
extern char arch_probe_write_resume[];
extern char arch_probe_exec_resume[];

/* 打印并修正保护状态；返回失败计数。 */
int arch_mmu_verify(void);

#endif /* LITEOS_ARCH_H */
