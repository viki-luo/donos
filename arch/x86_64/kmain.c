/*
 * kmain.c — 内核入口
 *
 * 目的：验证启动链路。当前刻意只用纯 ASCII 输出 ——
 * 这样一旦出现乱码，就能确定是执行问题而不是编码问题。
 * 中文输出等链路稳定后再加回。
 */
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define COM1 0x3F8

static inline void outb(u16 port, u8 val)
{
        __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port)
{
        u8 ret;
        __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
        return ret;
}

static void serial_putc(char c)
{
        while (!(inb(COM1 + 5) & 0x20))
                ;
        outb(COM1, (u8)c);
}

static void serial_puts(const char *s)
{
        while (*s) {
                if (*s == '\n')
                        serial_putc('\r');
                serial_putc(*s++);
        }
}

static void serial_hex(u32 v)
{
        for (int i = 28; i >= 0; i -= 4) {
                int nib = (v >> i) & 0xF;
                serial_putc(nib < 10 ? (char)('0' + nib) : (char)('a' + nib - 10));
        }
}

void kernel_main(u32 magic, u32 info)
{
        serial_puts("K\n");
        serial_puts("=== liteOS boot ===\n");

        serial_puts("[boot] magic = 0x");
        serial_hex(magic);
        serial_puts("\n");

        serial_puts("[boot] info  = 0x");
        serial_hex(info);
        serial_puts("\n");

        serial_puts("[base] not initialized yet\n");
        serial_puts("[idle] halting\n");

        for (;;)
                __asm__ volatile("hlt");
}