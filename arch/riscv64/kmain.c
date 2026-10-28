/*
 * kmain.c — riscv64 内核入口（QEMU virt + OpenSBI）
 *
 * 与 x86_64 的入口做同一件事，只是平台不同：
 *   1. 基座与物件侧初始化
 *   2. 验证：原语语义 + 统一模型 + R8 + 参照系 + 执行模型
 *   3. 运行：time CSR 驱动执行层，物件随真实时间运动
 *
 * base/ 下的代码与 x86_64 完全相同，一行未改。
 */
#include "arch.h"
#include "base.h"
#include "bus.h"
#include "object.h"
#include "verify.h"

#define STEP_NS 10000000ULL     /* 10 ms，与 x86_64 的 PIT 100 Hz 一致 */

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

        arch_puts("\n--- live: time CSR drives the execution layer ---\n");

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

        u64 start = arch_time_ns();
        u64 last = 0;
        bool reported = false;

        /*
         * 尚未接入定时器中断，主循环轮询 time CSR。
         * 每过一个步长调用一次 object_run，与 x86_64 的节拍语义一致。
         */
        for (;;) {
                u64 now = arch_time_ns() - start;
                u64 step = now / STEP_NS;
                if (step == last)
                        continue;
                last = step;

                object_run(step * STEP_NS);

                if (!reported && now >= 1000000000ULL) {
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
