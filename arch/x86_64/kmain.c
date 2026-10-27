/*
 * kmain.c — 内核入口
 *
 * 启动后立刻跑一遍基座自检，验证核心语义。
 * 输出刻意只用 ASCII：一旦出现乱码就能确定是执行问题而不是编码问题。
 */
#include "base.h"
#include "object.h"

/* ------------------------------------------------------------------ */
/* 串口（COM1）—— 已在 boot.asm 中初始化                              */
/* ------------------------------------------------------------------ */

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
 * 需要打印大数时按 32 位分段。
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

static void puthex(u32 v)
{
        for (int i = 28; i >= 0; i -= 4) {
                int nib = (v >> i) & 0xF;
                serial_putc(nib < 10 ? (char)('0' + nib) : (char)('a' + nib - 10));
        }
}

/* ------------------------------------------------------------------ */
/* 自检                                                                */
/* ------------------------------------------------------------------ */

static motion_event_t make_event(wire_t wire, wire_t frame, u64 dur,
                                 u8 dof, motion_fixed_t m0, motion_fixed_t m1)
{
        motion_event_t ev;
        ev.wire = wire;
        ev.frame = frame;
        ev.duration_ns = dur;
        ev.dof = dof;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = 0;
        ev.motion[0] = m0;
        ev.motion[1] = m1;
        return ev;
}

static int failures;

static void check(const char *what, bool ok)
{
        arch_puts(ok ? "  ok   " : "  FAIL ");
        arch_puts(what);
        arch_puts("\n");
        if (!ok)
                failures++;
}

static void selftest(void)
{
        arch_puts("\n--- base self-test ---\n");

        wire_t a = object_plug(2, 1000000);
        wire_t b = object_plug(2, 1000000);

        arch_puts("  A = wire "); arch_putdec(a);
        arch_puts(", B = wire "); arch_putdec(b);
        arch_puts("\n\n");

        /* 1. A 相对世界以 100 运动 */
        motion_event_t ev = make_event(a, FRAME_WORLD, 1000000000ULL, 1, 100, 0);
        check("A moves relative to the world",
              base_apply(&ev) == MOTION_OK && object_event_count(a) == 1);
        check("A is not still", !object_is_still(a));

        /*
         * 2. B 相对 A 静止 —— 这就是「静止是一种相对的运动」。
         *    注意没有单独的「停止」原语：这就是一个 motion 取零的事件。
         */
        ev = make_event(b, a, 1000000000ULL, 1, 0, 0);
        check("B is still relative to A (motion == 0)",
              base_apply(&ev) == MOTION_OK);
        check("B reads as still", object_is_still(b));

        /* 3. 参照系必须是一条有效的接线 */
        ev = make_event(a, 9999, 1000000000ULL, 1, 5, 0);
        check("unknown frame is rejected", base_apply(&ev) == MOTION_E_NO_FRAME);

        /* 4. 目标接线必须存在 */
        ev = make_event(9999, FRAME_WORLD, 1000000000ULL, 1, 5, 0);
        check("unknown wire is rejected", base_apply(&ev) == MOTION_E_NO_WIRE);

        /* 5. dof 越界 */
        ev = make_event(a, FRAME_WORLD, 1000000000ULL, MOTION_DOF_MAX + 1, 1, 0);
        check("dof over the limit is rejected", base_apply(&ev) == MOTION_E_INVAL);

        /* 6. 空事件指针 */
        check("null event is rejected", base_apply(0) == MOTION_E_INVAL);

        /* 7. 拔出后，接线立即失效，事件被拒 */
        object_unplug(b);
        check("detached wire is no longer valid", !base_wire_valid(b));
        ev = make_event(b, FRAME_WORLD, 1000000000ULL, 1, 7, 0);
        check("event to a detached wire is rejected",
              base_apply(&ev) == MOTION_E_NO_WIRE);

        /* 8. 拔出 B 之后，以 B 为参照系也必须被拒 */
        ev = make_event(a, b, 1000000000ULL, 1, 5, 0);
        check("a detached frame is rejected", base_apply(&ev) == MOTION_E_NO_FRAME);

        arch_puts("\n  failures = ");
        arch_putdec((u64)failures);
        arch_puts("\n");
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

void kernel_main(u32 magic, u32 info)
{
        arch_puts("=== liteOS ===\n");

        arch_puts("[boot] magic = 0x");
        puthex(magic);
        arch_puts("\n[boot] info  = 0x");
        puthex(info);
        arch_puts("\n");

        base_init();
        object_init();

        selftest();

        arch_puts("\n[idle] halting\n");

        for (;;)
                __asm__ volatile("hlt");
}