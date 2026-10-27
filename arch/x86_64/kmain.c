/*
 * kmain.c — 内核入口
 *
 * 只做启动编排，不含任何语义代码：
 *   1. 基座与物件侧初始化
 *   2. 平台：IDT → PIC → PIT → 开中断
 *   3. 总线发现：接线由此自动产生
 *   4. 验证：原语语义 + 统一模型
 *   5. 运行：PIT 节拍驱动执行层，物件随真实时间运动
 *
 * 语义验证在 base/verify.c —— 那是关于「模型成立与否」的检查，
 * 不该和「这台机器怎么启动」混在一起。
 */
#include "arch.h"
#include "base.h"
#include "bus.h"
#include "object.h"
#include "verify.h"

/* PIT 频率与每个节拍对应的纳秒数 */
#define TICK_HZ     100u
#define NS_PER_TICK (1000000000ULL / TICK_HZ)

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
        arch_pit_init(TICK_HZ);
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

        /* ---- 4. 验证 ---- */
        base_verify();

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

        /*
         * ---- 5. 运行：PIT 节拍驱动执行层 ----
         *
         * 中断处理只累加节拍；object_run 在这里的主循环中调用，
         * 不在中断上下文里跑物件的运动环，中断保持短小、不重入。
         */
        arch_puts("\n--- live: PIT ticks drive the execution layer ---\n");

        wire_t arm  = object_plug(2, NS_PER_TICK);
        wire_t hand = object_plug(2, NS_PER_TICK);

        motion_event_t ev;
        ev.duration_ns = 1000000000ULL;
        ev.dof = 1;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = 0;

        ev.wire = arm;                  /* arm 相对世界每步 3 */
        ev.frame = FRAME_WORLD;
        ev.motion[0] = 3;
        base_apply(&ev);

        ev.wire = hand;                 /* hand 相对 arm 静止 */
        ev.frame = arm;
        ev.motion[0] = 0;
        base_apply(&ev);

        u64 start = arch_ticks();
        u64 last = start;
        bool reported = false;

        for (;;) {
                __asm__ volatile("hlt");

                u64 now = arch_ticks();
                if (now == last)
                        continue;
                last = now;

                object_run(now * NS_PER_TICK);

                if (!reported && now - start >= TICK_HZ) {
                        object_state_t st;

                        arch_puts("  after 1 s of real time:\n");
                        arch_puts("    arm  steps = ");
                        arch_putdec(object_step_count(arm));
                        if (object_state(arm, &st)) {
                                arch_puts(", position = ");
                                arch_putdec((u64)st.position[0]);
                        }
                        arch_puts("\n    hand steps = ");
                        arch_putdec(object_step_count(hand));
                        if (object_state(hand, &st)) {
                                arch_puts(", position = ");
                                arch_putdec((u64)st.position[0]);
                        }
                        arch_puts("  (still relative to arm)\n");
                        arch_puts("\n[live] objects keep moving; the base stays idle\n");
                        reported = true;
                }
        }
}
