/*
 * console.c — riscv64 串口控制台（QEMU virt 的 16550 UART）
 *
 * 与 x86_64 的 console.c 对外是同一组函数，只是寄存器从端口 I/O
 * 换成了内存映射。基座通过 report.h 调用它们，看不出区别。
 */
#include "arch.h"

#define UART_BASE 0x10000000UL
#define UART_THR  0x00
#define UART_LSR  0x05
#define LSR_THRE  0x20

static inline void mmio_write8(unsigned long addr, u8 v)
{
        *(volatile u8 *)addr = v;
}

static inline u8 mmio_read8(unsigned long addr)
{
        return *(volatile u8 *)addr;
}

static void serial_putc(char c)
{
        while (!(mmio_read8(UART_BASE + UART_LSR) & LSR_THRE))
                ;
        mmio_write8(UART_BASE + UART_THR, (u8)c);
}

void arch_puts(const char *s)
{
        while (*s) {
                if (*s == '\n')
                        serial_putc('\r');
                serial_putc(*s++);
        }
}

void arch_putdec(u64 value)
{
        char buf[21];
        int n = 0;

        if (value == 0) {
                serial_putc('0');
                return;
        }
        while (value > 0) {
                buf[n++] = (char)('0' + (value % 10u));
                value /= 10u;
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
        arch_puthex32((u32)(v >> 32));
        arch_puthex32((u32)(v & 0xFFFFFFFFu));
}

/* 测量用周期计数：time CSR 的计数（10 MHz） */
u64 arch_cycles(void)
{
        return arch_time();
}

/*
 * 保护状态的自我报告。
 *
 * satp 编码：MODE 在最高位。RV64 下 MODE = 0 为 Bare（不做翻译），
 * 8 为 Sv39，9 为 Sv48，10 为 Sv57。这里只需要看它是不是 0。
 */
void arch_report_protection(void)
{
        u64 satp = 0;

        __asm__ volatile("csrr %0, satp" : "=r"(satp));

        arch_puts("  satp = 0x");
        arch_puthex64(satp);
        arch_puts("  (MODE=");
        arch_putdec(satp >> 60);
        arch_puts(" ");

        if (satp == 0)
                arch_puts("Bare: no translation, no permission bits");
        else
                arch_puts("translation active");

        arch_puts(")\n");
        arch_puts("  => memory is translated in S-mode; there is no user mode here,\n");
        arch_puts("     so an upper brain runs with the same privilege as the base.\n");
}
