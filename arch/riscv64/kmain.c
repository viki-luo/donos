/*
 * kmain.c — riscv64 内核入口（QEMU virt + OpenSBI）
 *
 * 与 x86_64 的入口做同一件事，只是平台不同：
 *   1. 基座与物件侧初始化
 *   2. 验证：原语语义 + 统一模型 + R8 + 参照系 + 执行模型
 *   3. 运行：定时器中断累加节拍，节拍驱动执行层，物件随真实时间运动
 *
 * base/ 下的代码与 x86_64 完全相同，一行未改。
 */
#include "arch.h"
#include "base.h"
#include "bench.h"
#include "bus.h"
#include "object.h"
#include "verify.h"

/* 10 ms，与 x86_64 的 PIT 100 Hz 一致；由定时器中断的节拍决定 */
#define STEP_NS ARCH_NS_PER_TICK

void kernel_main(u64 hartid, u64 dtb)
{
        arch_puts("=== liteOS (riscv64) ===\n");
        arch_puts("[boot] hart = ");
        arch_putdec(hartid);
        arch_puts("\n[boot] dtb  = 0x");
        arch_puthex64(dtb);
        arch_puts("\n");

        base_init();
        object_init();

        /* 总线发现：与 x86_64 共用 drivers/pci.c，配置空间走 ECAM */
        pci_discover();

        /* 保护状态自报：satp 是不是 0，决定「有没有保护」这件事 */
        arch_puts("\n--- protection: what the MMU actually enforces ---\n");
        arch_report_protection();

        base_verify();

        /*
         * 定时器必须在测量之前启动：bench 的「一个节拍值多少个计数」
         * 靠等待真实节拍来测，节拍没起来就会永远等下去。
         */
        arch_timer_init(hartid);
        arch_irq_enable();

        /* 测量：原语代价、参照链代价、最坏节拍占用、压力行为 */
        bench_run();

        /*
         * 重新装订定时器，丢弃基准期间的积压。
         *
         * 一次性定时器按「计划时刻」累加 next_cmp：基准忙着算不回到主循环时，
         * 多个截止时刻已经过期，中断会背靠背触发，节拍一次跳好几位，
         * 而主循环只推进了一次 —— 于是节拍数多于步数。
         * x86_64 的 PIT 是周期性的，没有这个问题。
         * 重新装订让第一个真实节拍从现在起算，两边再对齐。
         */
        arch_timer_init(hartid);
        arch_irq_enable();

        arch_puts("\n--- live: timer interrupts drive the execution layer ---\n");

        wire_t arm  = object_plug(2, STEP_NS);
        wire_t hand = object_plug(2, STEP_NS);

        motion_event_t ev;
        /*
         * 演示事件永不过期（issued = 0，窗口取满）：现场演示关注的是
         * 「接受后自持」，新鲜度语义由 verify_freshness 专门验证。
         */
        ev.issued_at_ns = 0;
        ev.duration_ns = UINT64_MAX;
        ev.dof = 1;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = 0;

        /*
         * 提交时刻由调用者给出。
         *
         * 必须是执行层时间轴上的「现在」，不能用 arch_ticks() * STEP_NS：
         * ARCH 的节拍计数从 0 起，而自检/基准已经把执行时钟推到了 500 ms 之后，
         * 两条时间轴直接混用就是一次真实的时间回退，事件会被正确地判为
         * 「发出于未来」而拒绝。
         */
        u64 submit_at = object_now_ns();

        ev.wire = arm;                  /* arm 相对世界 3 m/s（决定 18） */
        ev.frame = FRAME_WORLD;
        ev.motion[0] = (motion_fixed_t)3 << 32;
        base_apply(submit_at, &ev);

        ev.wire = hand;                 /* hand 相对 arm 静止 */
        ev.frame = arm;
        ev.motion[0] = 0;
        base_apply(submit_at, &ev);

        bool reported = false;

        /*
         * 与 x86_64 同一套语义：中断只累加节拍，推进执行层留给主循环。
         * 区别是这里不必轮询 time CSR —— 主循环在 wfi 里等中断。
         */
        u64 start = arch_ticks();
        u64 last = start;

        for (;;) {
                /*
                 * 关中断读节拍：若在「读节拍」与「wfi」之间来了中断，那个
                 * 节拍就会被跳过（丢唤醒竞态），节拍数会比步数多一。
                 * 关掉 sstatus.SIE 后 wfi 仍会被挂起的中断唤醒 —— 只是不
                 * 进陷阱处理 —— 重新打开时它立刻被受理，因此既不丢也不睡死。
                 */
                arch_irq_disable();

                u64 ticks = arch_ticks();
                if (ticks == last) {
                        __asm__ volatile("wfi");
                        arch_irq_enable();
                        continue;
                }

                arch_irq_enable();

                /*
                 * 只在执行层自己的时间轴上推进：object_now_ns() 是上一次
                 * 被接受的时刻，加上本循环实测的节拍增量必然严格前进。
                 * ARCH 的节拍计数从 0 起，与自检/基准推到的时刻不是同一条
                 * 时间轴，直接拿它当执行时刻就是一次真实的时间回退。
                 */
                u64 advance = (ticks - last) * STEP_NS;
                last = ticks;

                u32 stepped = 0;

                if (object_run(object_now_ns() + advance, &stepped) !=
                    OBJECT_RUN_OK) {
                        arch_puts("  [live] execution clock refused this "
                                  "instant; no object was advanced\n");
                }

                if (!reported && (ticks - start) * STEP_NS >= 1000000000ULL) {
                        object_state_t st;

                        arch_puts("  after 1 s of real time:\n");
                        arch_puts("    ticks    = ");
                        arch_putdec(ticks - start);
                        arch_puts("\n    arm  steps = ");
                        arch_putdec(object_step_count(arm));
                        if (object_state(arm, &st)) {
                                arch_puts(", position (m) = ");
                                arch_putdec((u64)(st.position[0] >> 32));
                        }
                        arch_puts("\n    hand steps = ");
                        arch_putdec(object_step_count(hand));
                        if (object_state(hand, &st)) {
                                arch_puts(", position (m) = ");
                                arch_putdec((u64)(st.position[0] >> 32));
                        }
                        arch_puts("  (still relative to arm)\n");
                        if (object_step_count(arm) != ticks - start)
                                arch_puts("  note: one tick fewer than ticks elapsed, because the\n"
                                          "        first sighting only schedules (no free step),\n"
                                          "        and a late run does not catch up (decision 16)\n");
                        arch_puts("\n[live] objects keep moving; the base stays idle\n");
                        reported = true;
                }
        }
}
