/*
 * kmain.c — 内核入口
 *
 * 启动顺序：
 *   1. 基座与物件侧初始化（清空接线表与物件表）
 *   2. 平台：IDT → PIC → PIT → 开中断
 *   3. 总线发现：PCI 扫描，接线由此自动产生
 *   4. 语义自检：验证「一切皆运动」的几条不变量
 *
 * 第 3 步是关键变化：在这之前，接线是测试代码里手写 object_plug 造出来的；
 * 现在它来自真实的硬件扫描。
 */
#include "arch.h"
#include "base.h"
#include "bus.h"
#include "object.h"

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
        arch_puts("\n--- base self-test (synthetic objects) ---\n");

        /*
         * 这两条接线是自检专用的合成物件，用来验证语义。
         * 真实物件的接线来自上一步的总线发现。
         */
        wire_t a = object_plug(2, 1000000);
        wire_t b = object_plug(2, 1000000);

        arch_puts("  A = wire ");
        arch_putdec(a);
        arch_puts(", B = wire ");
        arch_putdec(b);
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
        arch_puthex32(magic);
        arch_puts("\n[boot] info  = 0x");
        arch_puthex32(info);
        arch_puts("\n");

        /* ---- 1. 基座与物件侧 ---- */
        base_init();
        object_init();

        /* ---- 2. 平台 ---- */
        arch_puts("\n--- platform ---\n");
        arch_idt_init();
        arch_puts("  idt   installed (48 vectors)\n");
        arch_pic_init();
        arch_puts("  pic   remapped to 0x20/0x28, only IRQ0 open\n");
        arch_pit_init(100);
        arch_puts("  pit   100 Hz\n");

        /*
         * 开中断，并等到第一个节拍到来 —— 这样「定时器真的在跑」
         * 是被证实的事实，不是一个假设。
         */
        arch_irq_enable();
        u64 t0 = arch_ticks();
        while (arch_ticks() == t0)
                __asm__ volatile("hlt");
        arch_puts("  irq   enabled, timer ticking\n");

        /* ---- 3. 总线发现：接线由此产生 ---- */
        pci_discover();

        /* ---- 4. 语义自检 ---- */
        selftest();

        /*
         * 可选的故障注入：make FAULT_TEST=1
         *
         * 碰一段恒等映射之外的内存，触发页错误，用来验证异常处理器
         * 真的会打印现场。没有这一步，「装了 IDT」和「IDT 有用」
         * 只是两个不同的说法。
         */
#ifdef FAULT_TEST
        arch_puts("\n[fault-test] touching unmapped memory at 0x40000000...\n");
        *(volatile u64 *)0x40000000ULL = 1;
        arch_puts("[fault-test] NOT REACHED: handler did not fire\n");
#endif

        arch_puts("\n[idle] halting\n");

        for (;;)
                __asm__ volatile("hlt");
}
