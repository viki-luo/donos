/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * trap.c — riscv64 陷阱处理（QEMU virt + OpenSBI）
 *
 * 为什么需要这一层：没有它，任何异常都会让内核看起来像卡死或重启。
 * 有了它，异常会打印 scause / sepc / stval 后停机，定时器中断才能累加节拍。
 *
 * 定时器用 CLINT 的 mtimecmp：写完下一次比较值后返回，由 sret 回到被
 * 中断的代码。基座与执行层都不知道中断的存在。
 */
#include "arch.h"

/*
 * 定时器必须通过 SBI 调用设置，不能直接写 CLINT 的 mtimecmp。
 *
 * 原因不是「不方便」，而是权限：OpenSBI 用 PMP 把 CLINT（0x02000000，
 * 32 KiB）设为 S 态无任何访问权限 —— 实测 pmpcfg0 的第 4 个条目
 * （0x18，NAPOT，R=W=X=0），而覆盖全地址空间的高编号条目 RWX 优先级
 * 更低。S 态直接读 mtime 得到 scause 5，直接写 mtimecmp 得到 scause 7。
 *
 * 因此用 SBI TIME 扩展（EID 0x54494D45，FID 0）请求 M 态代为编程。
 * time CSR 本身仍可读（mcounteren 已放开），用它取绝对时刻。
 */
#define SBI_EID_TIME      0x54494D45UL   /* "TIME" */
#define SBI_EID_TIME_OLD  0x00UL         /* 旧式 set_timer */

#define SCAUSE_INT     0x8000000000000000ULL
#define SCAUSE_CODE    0xFFULL
#define IRQ_S_TIMER    5

static volatile u64 tick_count;

extern void trap_entry(void);

typedef struct {
        volatile bool armed;
        volatile bool seen;
        u64 cause;
        u64 sepc;
        u64 stval;
        u64 resume;
        u64 observed_cause;
        u64 observed_sepc;
        u64 observed_stval;
} expected_fault_t;

static expected_fault_t expected_fault;

void arch_trap_init(void)
{
        u64 vec = (u64)(uintptr_t)trap_entry;
        __asm__ volatile("csrw stvec, %0" :: "r"(vec) : "memory");
}

void arch_fault_expect(u64 cause, u64 sepc, u64 stval, u64 resume)
{
        expected_fault.seen = false;
        expected_fault.cause = cause;
        expected_fault.sepc = sepc;
        expected_fault.stval = stval;
        expected_fault.resume = resume;
        expected_fault.observed_cause = 0;
        expected_fault.observed_sepc = 0;
        expected_fault.observed_stval = 0;
        __asm__ volatile("" ::: "memory");
        expected_fault.armed = true;
}

bool arch_fault_result(u64 *cause, u64 *sepc, u64 *stval)
{
        __asm__ volatile("" ::: "memory");
        if (cause != 0)
                *cause = expected_fault.observed_cause;
        if (sepc != 0)
                *sepc = expected_fault.observed_sepc;
        if (stval != 0)
                *stval = expected_fault.observed_stval;
        return expected_fault.seen && !expected_fault.armed;
}

static void halt(void)
{
        for (;;)
                __asm__ volatile("wfi");
}

/*
 * 请求 M 态把下一次定时器中断设在绝对时刻 when（time 计数单位）。
 * 返回 0 表示成功。
 */
static long sbi_set_timer(u64 when)
{
        register long a0 __asm__("a0") = (long)when;
        register long a6 __asm__("a6") = 0;
        register long a7 __asm__("a7") = (long)SBI_EID_TIME;

        __asm__ volatile("ecall"
                         : "+r"(a0)
                         : "r"(a6), "r"(a7)
                         : "memory");
        return a0;
}

/* 太旧的 OpenSBI 不认 TIME 扩展，退回旧式 EID 0；只在首次失败时报一次。 */
static bool sbi_reported;

static void timer_set(u64 when)
{
        if (sbi_set_timer(when) == 0)
                return;

        register long a0 __asm__("a0") = (long)when;
        register long a7 __asm__("a7") = (long)SBI_EID_TIME_OLD;
        __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");

        if (a0 != 0 && !sbi_reported) {
                arch_puts("!! SBI set_timer failed, timer will not fire\n");
                sbi_reported = true;
        }
}

/*
 * 下一次性比较值由上一次推算，不读 mtime：省下一次 SBI 调用，
 * 也让节拍间隔不受中断响应延迟影响。
 * tick_cycles 是当前的节拍宽度：额定值 ARCH_TICK_CYCLES，
 * 中断风暴测试经 arch_timer_hz() 临时调小、再恢复。
 */
static u64 next_cmp;
static u64 tick_cycles = ARCH_TICK_CYCLES;

void arch_timer_hz(u32 hz)
{
        if (hz == 0)
                hz = 1;
        if ((u64)hz > ARCH_TIME_HZ)
                hz = (u32)ARCH_TIME_HZ;

        tick_cycles = ARCH_TIME_HZ / hz;
        if (tick_cycles == 0)
                tick_cycles = 1;

        /* 让新宽度立即生效：从「现在」重新装订一次。 */
        next_cmp = arch_time() + tick_cycles;
        timer_set(next_cmp);
}

void arch_timer_init(u64 hart)
{
        (void)hart;                     /* 单 hart 运行，SBI 调用隐式使用它 */

        /*
         * 静态初值必须在引导清零之后仍然存在。
         *
         * riscv64 的 .data 与 .bss 共处一个输出段；早先 __bss_start 被定义
         * 在 .data 的开头，boot.S 的清零循环于是把整个 .data 的初值抹掉。
         * 这类缺陷平时无声，只有在「真的存在非零初值」时才显形：
         * tick_cycles 被清零后节拍宽度为 0，截止时刻永不过期，
         * 中断变成无限风暴，主循环被彻底饿死。
         * 因此这里显式核对一次，并把结论打印成门禁可抓的标记。
         */
        if (tick_cycles != ARCH_TICK_CYCLES) {
                arch_puts("  !! .data initializer lost: tick_cycles = ");
                arch_putdec(tick_cycles);
                arch_puts(" (expected ");
                arch_putdec(ARCH_TICK_CYCLES);
                arch_puts(")\n");
                tick_cycles = ARCH_TICK_CYCLES;  /* 先让系统能跑，错误已报出 */
        } else {
                arch_puts("  ok   .data initializer survived boot\n");
        }

        /* 陷阱向量必须先装好，再打开中断。 */
        arch_trap_init();

        next_cmp = arch_time() + ARCH_TICK_CYCLES;
        timer_set(next_cmp);

        /* sie.STIE = bit 5：允许 S 态定时器中断 */
        __asm__ volatile("li t0, 32\n\tcsrs sie, t0" ::: "t0");
}

void arch_irq_enable(void)
{
        /* sstatus.SIE = bit 1 */
        __asm__ volatile("li t0, 2\n\tcsrs sstatus, t0" ::: "t0");
}

void arch_irq_disable(void)
{
        __asm__ volatile("li t0, 2\n\tcsrc sstatus, t0" ::: "t0");
}

u64 arch_ticks(void)
{
        return tick_count;
}

/*
 * 由 traps.S 调用。scause 最高位为 1 表示中断，否则为异常。
 */
u64 trap_dispatch(u64 scause, u64 sepc, u64 stval)
{
        if (scause & SCAUSE_INT) {
                u64 code = scause & SCAUSE_CODE;

                if (code == IRQ_S_TIMER) {
                        tick_count++;
                        next_cmp += tick_cycles;
                        timer_set(next_cmp);
                        return sepc;
                }

                arch_puts("\n!! unexpected interrupt, code = ");
                arch_putdec(code);
                arch_puts("\n");
                halt();
        }

        if (expected_fault.armed) {
                expected_fault.observed_cause = scause;
                expected_fault.observed_sepc = sepc;
                expected_fault.observed_stval = stval;

                if (scause == expected_fault.cause &&
                    sepc == expected_fault.sepc &&
                    stval == expected_fault.stval) {
                        u64 resume = expected_fault.resume;
                        expected_fault.armed = false;
                        expected_fault.seen = true;
                        __asm__ volatile("" ::: "memory");
                        return resume;
                }

                arch_puts("\n!! expected MMU fault mismatch");
                expected_fault.armed = false;
        }

        arch_puts("\n!! CPU exception\n  scause = 0x");
        arch_puthex64(scause);
        arch_puts("\n  sepc   = 0x");
        arch_puthex64(sepc);
        arch_puts("\n  stval  = 0x");
        arch_puthex64(stval);
        arch_puts("\n");

        halt();
        return sepc;
}
