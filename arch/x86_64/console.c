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
