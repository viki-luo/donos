/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * console.c — 串口控制台
 *
 * 只用 ASCII 输出。出现乱码时能立刻判断是执行问题而不是编码问题 ——
 * 这在裸机调试里很省时间（曾因把中文串走串口而输出成乱码）。
 *
 * COM1 的初始化在引导阶段完成（boot.asm），这里只负责收发。
 */
#include "arch.h"

#define COM1 0x3F8

static void serial_putc(char c)
{
        while (!(arch_inb(COM1 + 5) & 0x20))
                ;
        arch_outb(COM1, (u8)c);
}

void arch_puts(const char *s)
{
        while (*s) {
                if (*s == '\n')
                        serial_putc('\r');
                serial_putc(*s++);
        }
}

/*
 * 十进制输出。
 *
 * 刻意用 u32 而不是 u64：64 位除法在 freestanding 下会被编译成
 * __udivdi3 调用，而这里没有链接编译器运行库。
 */
void arch_putdec(u64 value)
{
        u32 v = (u32)value;
        char buf[12];
        int n = 0;

        if (v == 0) {
                serial_putc('0');
                return;
        }
        while (v > 0) {
                buf[n++] = (char)('0' + (v % 10u));
                v /= 10u;
        }
        while (n > 0)
                serial_putc(buf[--n]);
}

static void put_nibble(u32 nib)
{
        serial_putc(nib < 10 ? (char)('0' + nib) : (char)('a' + nib - 10));
}

void arch_puthex32(u32 v)
{
        for (int i = 28; i >= 0; i -= 4)
                put_nibble((v >> i) & 0xF);
}

void arch_puthex64(u64 v)
{
        /* 分两半，避免 64 位移位在 32 位运算上的歧义 */
        arch_puthex32((u32)(v >> 32));
        arch_puthex32((u32)(v & 0xFFFFFFFFu));
}

/*
 * 测量用周期计数：读取 TSC。
 */
u64 arch_cycles(void)
{
        u32 lo, hi;

        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return ((u64)hi << 32) | (u64)lo;
}

/*
 * 保护状态的自我报告。
 *
 * 引导阶段建的是恒等映射：pml4 0x70000 / pdpt 0x71000 / pd 0x72000。
 * 页目录项用 2 MiB 大页（PS 位），所以直接读 PD 就能看出权限位。
 *
 * 这里要回答的问题不是「有没有分页」，而是「分页有没有提供保护」。
 * 打印出来的位自己会说话：
 *   bit 0  P    存在
 *   bit 1  RW   可写
 *   bit 2  US   用户态可访问
 *   bit 7  PS   大页
 *   bit 63 NX   不可执行
 */
void arch_report_protection(void)
{
        u64 cr0, cr4, cr3;

        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

        arch_puts("  cr0 = 0x");
        arch_puthex64(cr0);
        arch_puts("  (PG=");
        arch_puts((cr0 & 0x80000000u) ? "1" : "0");
        arch_puts(" WP=");
        arch_puts((cr0 & 0x00010000u) ? "1" : "0");
        arch_puts(")\n");

        arch_puts("  cr4 = 0x");
        arch_puthex64(cr4);
        arch_puts("  (PAE=");
        arch_puts((cr4 & 0x20u) ? "1" : "0");
        arch_puts(")\n");

        arch_puts("  cr3 = 0x");
        arch_puthex64(cr3);
        arch_puts("\n");

        const volatile u64 *pd = (const volatile u64 *)0x72000ULL;
        for (u32 i = 0; i < 2u; i++) {
                u64 e = pd[i];

                arch_puts("  pd[");
                arch_putdec(i);
                arch_puts("] = 0x");
                arch_puthex64(e);
                arch_puts("  (P=");
                arch_puts((e & 1u) ? "1" : "0");
                arch_puts(" RW=");
                arch_puts((e & 2u) ? "1" : "0");
                arch_puts(" US=");
                arch_puts((e & 4u) ? "1" : "0");
                arch_puts(" NX=");
                arch_puts(((e >> 63) & 1u) ? "1" : "0");
                arch_puts(")\n");
        }

        arch_puts("  => identity mapped: every page writable and executable,\n");
        arch_puts("     no user/supervisor split. The base is not sandboxed by the MMU.\n");
}
