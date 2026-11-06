/*
 * cpuid.c — CPUID 能力探测（x86_64 架构层）
 *
 * 为什么在页表阶段之前先做这个：
 *
 *   「支持 NX」与「NX 现在生效」是两件不同的事。NXE 是 EFER 里的一个位，
 *   没有 CPUID.8000_0001:EDX[20] 就置不上去；而在 CPUID 叶子不可用时，
 *   CPUID 指令本身会返回垃圾而不是报错 —— 直接相信返回值，等于把
 *   「我不知道」读成「我支持」。
 *
 *   所以这里只回答基座之外的问题：这台机器有没有那个位。
 *   能不能用、用不用，由页表阶段决定。
 */
#include "arch.h"

void arch_cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
        __asm__ volatile("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(0));
}

/*
 * 探测 NX（no-execute）是否可用。
 *
 * 两点都不能省：
 *   - 先读 CPUID.8000_0000:EAX，确认 0x80000001 这个叶子**存在**。
 *     扩展叶子的最大号低于它时，读 0x80000001 得到的是最后一个叶子
 *     的重复，EDX[20] 可能恰好为 1 —— 于是「不支持」被读成「支持」。
 *   - 再看 EDX[20]，那才是真正的那一位。
 */
bool arch_cpu_has_nx(void)
{
        u32 a, b, c, dup;

        arch_cpuid(0x80000000u, &a, &b, &c, &dup);

        if (a < 0x80000001u)
                return false;

        u32 d;

        arch_cpuid(0x80000001u, &a, &b, &c, &d);
        return (d & (1u << 20)) != 0;
}

/*
 * CPUID 是不是**根本没实现**。
 *
 * 老于 486 的机器上 CPUID 是一条无效指令，我们不会在那种机器上跑；
 * 但虚拟化环境把 CPUID 屏蔽掉是另一回事，那会让本文件返回「不支持」，
 * 而不是直接把启动打死 —— 这两种情况的处置完全不同，所以分开判。
 */
bool arch_cpu_has_cpuid(void)
{
        u32 before, after;

        __asm__ volatile("pushfq\n\t"
                         "pop  %%rax\n\t"
                         "mov  %%eax, %0\n\t"
                         "xor  %%eax, 0x200000\n\t"
                         "push %%rax\n\t"
                         "popfq\n\t"
                         "pushfq\n\t"
                         "pop  %%rax\n\t"
                         "mov  %%eax, %1\n\t"
                         : "=r"(before), "=r"(after)
                         :
                         : "eax", "cc");

        return (before ^ after) & 0x200000u;
}
