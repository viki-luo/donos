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

        base_verify();

        arch_puts("\n--- live: timer interrupts drive the execution layer ---\n");

        wire_t arm  = object_plug(2, STEP_NS);
        wire_t hand = object_plug(2, STEP_NS);

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

        bool reported = false;

        /*
         * 与 x86_64 同一套语义：中断只累加节拍，推进执行层留给主循环。
         * 区别是这里不必轮询 time CSR —— 主循环在 wfi 里等中断。
         */
        arch_timer_init(hartid);
        arch_irq_enable();

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
                last = ticks;

                /* 传绝对节拍时刻，与 x86_64 一致 */
                object_run(ticks * STEP_NS);

                if (!reported && (ticks - start) * STEP_NS >= 1000000000ULL) {
                        object_state_t st;

                        arch_puts("  after 1 s of real time:\n");
                        arch_puts("    ticks    = ");
                        arch_putdec(ticks - start);
                        arch_puts("\n    arm  steps = ");
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
