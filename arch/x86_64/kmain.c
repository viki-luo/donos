/* SPDX-License-Identifier: GPL-2.0-only */
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
#include "bench.h"
#include "brain.h"
#include "bus.h"
#include "monitor.h"
#include "object.h"
#include "verify.h"

/* PIT 频率与每个节拍对应的纳秒数 */
#define TICK_HZ     100u
#define NS_PER_TICK (1000000000ULL / TICK_HZ)

/*
 * 打印 Q32.32，带三位小数且保留符号。
 *
 * 只打整数部分是不行的：换向后的 -0.5 m/s 会打成「-0」，
 * 位置从 1.0 退到 0.5 会打成「1」—— 输出看起来像「命令没生效」，
 * 而实际生效了。演示必须让证据可读，否则它只是装饰。
 */
static void put_fixed(motion_fixed_t v)
{
        const motion_fixed_t one = 4294967296LL;
        bool neg = v < 0;
        motion_fixed_t a = neg ? -v : v;
        motion_fixed_t whole = a / one;
        motion_fixed_t frac = ((a % one) * 1000) / one;

        if (neg)
                arch_puts("-");
        arch_putdec((u64)whole);
        arch_puts(".");
        if (frac < 100)
                arch_puts("0");
        if (frac < 10)
                arch_puts("0");
        arch_putdec((u64)frac);
}

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

        /*
         * ---- 4. 保护状态自报 ----
         *
         * 「有分页」和「分页提供保护」是两件事。让机器自己把位打出来。
         */
        arch_puts("\n--- protection: what the MMU actually enforces ---\n");
        arch_report_protection();

        /*
         * ---- 4.5 MMU 隔离 ----
         *
         * 上一步打印的是**装保护之前**的事实。这一步把能装的装上：
         * NXE 置位、第 0 个大页拆成 4 KiB 页（代码 R|X、数据 R|W）、
         * 其余大页打 NX、CR0.WP 置位。
         *
         * 这条路径**无条件执行**：它不是测试，是产品的一部分。
         * 验证（故意违规访问）只在 LITEOS_SELFTEST=1 时编入。
         */
        if (!arch_mmu_enable())
                arch_puts("\n!! this CPU has no NX; W^X is not available\n");

#if LITEOS_SELFTEST
        if (arch_mmu_verify() != 0)
                arch_puts("\n!! MMU isolation did not take\n");
#endif

        /*
         * ---- 5. 验证与测量（仅自检构建） ----
         *
         * 自检是证明手段，不是产品：发布镜像不编入这些代码。
         */
#if LITEOS_SELFTEST
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
         * ---- 6. 测量 ----
         *
         * 全部在目标机上实测：原语代价、参照链代价、最坏节拍占用、
         * 以及压力下的行为（表满、反复插拔、伪造句柄）。
         */
        bench_run();

        /*
         * ---- 6.5 大脑热切换自检 ----
         *
         * 放在基准之后，因为它的时间基准取 object_now_ns()：基准已经把
         * 执行时钟推到很远，自检必须从那条时间轴上接着走，否则就是一次
         * 真实的时间回退。
         */
        brain_verify();
#endif

        /*
         * ---- 6.6 基座之外的监视器（仅自检构建） ----
         *
         * 只观察、不干预：它的出口是事件，不是命令。
         * 自检本身证明「一次轮询之后物件状态逐字段不变」。
         */
#if LITEOS_SELFTEST
        monitor_verify();
#endif

        /*
         * ---- 7. 运行：PIT 节拍驱动执行层 ----
         *
         * 中断处理只累加节拍；object_run 在这里的主循环中调用，
         * 不在中断上下文里跑物件的运动环，中断保持短小、不重入。
         */
        arch_puts("\n--- live: PIT ticks drive the execution layer ---\n");

        wire_t arm  = object_plug(2, NS_PER_TICK);
        wire_t hand = object_plug(2, NS_PER_TICK);

        /*
         * 被大脑驱动的那只物件。
         *
         * 它与 arm / hand 没有任何区别：同一套 object_plug，同一个句柄
         * 编码，基座里没有一个位是留给「被大脑驱动」的。
         */
        wire_t carriage = object_plug(2, NS_PER_TICK);

        brain_t *cruise = brain_cruise(carriage, NS_PER_TICK);
        brain_t *active = cruise;
        bool did_swap = false;
        bool did_report = false;
        u64 swap_tick = 0;

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
         * 必须是执行层时间轴上的「现在」，不能用 arch_ticks() * NS_PER_TICK：
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

        u64 start = arch_ticks();
        u64 last = start;
        bool reported = false;

        for (;;) {
                __asm__ volatile("hlt");

                u64 now = arch_ticks();
                if (now == last)
                        continue;

                /*
                 * 只在执行层自己的时间轴上推进：object_now_ns() 是上一次
                 * 被接受的时刻，加上本循环实测的节拍增量必然严格前进。
                 * 直接拿 arch_ticks() * NS_PER_TICK 当时间会与自检/基准
                 * 已经推到的时刻错位，那是一次真实的时间回退。
                 */
                u64 advance = (now - last) * NS_PER_TICK;
                last = now;

                u32 stepped = 0;

                if (object_run(object_now_ns() + advance, &stepped) !=
                    OBJECT_RUN_OK) {
                        arch_puts("  [live] execution clock refused this "
                                  "instant; no object was advanced\n");
                }

                /* 运行时热插拔：每 100 ms 重扫一次总线 */
                if (now % (TICK_HZ / 10) == 0)
                        pci_poll();

                /*
                 * ---- 大脑在跑 ----
                 *
                 * 这里发生的事情就是「上层大脑」的全部：它读现场、
                 * 调 base_apply。基座完全不知道驱动它的是谁。
                 */
                if (active != 0)
                        brain_tick(active, object_now_ns());

                /*
                 * ---- 大脑热切换 ----
                 *
                 * 2 秒时换成另一个控制律不同的大脑。这里**没有**任何
                 * 交接：新大脑第一次决策读的就是物件自持的位置。
                 * 目标物件不重置、不暂停，继续沿上一个大脑最后被接受
                 * 的命令运动，直到被新命令替换。
                 *
                 * 诚实边界：切换是**协作式**的。基座没有权限模型，
                 * 无法阻止旧大脑继续调用 base_apply —— 上一段代码里
                 * 那个迟到的过期事件之所以被拒，是因为它的新鲜度窗口
                 * 关了，而不是因为基座认得出「这是旧大脑」。
                 */
                if (reported && !did_swap && now - start >= 2 * TICK_HZ) {
                        object_state_t st;

                        arch_puts("\n[swap] detaching brain 'cruise', "
                                  "attaching 'shuttle'\n");

                        if (object_state(carriage, &st)) {
                                arch_puts("  carriage position at swap (m) = ");
                                put_fixed(st.position[0]);
                                arch_puts("\n  (the new brain will read this from "
                                          "the object, not from a handover)\n");
                        }

                        brain_detach(cruise);
                        active = brain_shuttle(carriage, NS_PER_TICK);
                        did_swap = true;
                        swap_tick = now;

                        if (active != 0) {
                                brain_tick(active, object_now_ns());
                                arch_puts("  new brain commanded (m/s) = ");
                                put_fixed(object_motion(carriage, 0));
                                arch_puts(", i.e. the opposite direction\n");
                        }
                }

                /*
                 * 切换之后满 1 秒再看结果。基准必须是**切换那一刻**，
                 * 不是启动那一刻：串口输出很慢，一次打印就可能跨过 1 秒，
                 * 按启动时刻判定会让报告紧接着 swap 打印出来 ——
                 * 那时命令还没跑过一步，看起来像「新大脑无效」。
                 */
                if (did_swap && !did_report && now - swap_tick >= TICK_HZ) {
                        object_state_t st;

                        if (object_state(carriage, &st)) {
                                arch_puts("  [swap] after 1 s under the new brain: "
                                          "carriage position (m) = ");
                                put_fixed(st.position[0]);
                                arch_puts(", command (m/s) = ");
                                put_fixed(object_motion(carriage, 0));
                                arch_puts(", steps = ");
                                arch_putdec(object_step_count(carriage));
                                arch_puts(", decisions = ");
                                arch_putdec(active ? active->decisions : 0);
                                arch_puts("\n  (one second at the new command "
                                          "moved it back 0.5 m; the base never\n"
                                          "   learned that a brain was "
                                          "replaced)\n");
                        }
                        did_report = true;
                }

                if (!reported && now - start >= TICK_HZ) {
                        object_state_t st;

                        arch_puts("  after 1 s of real time:\n");
                        arch_puts("    arm  steps = ");
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
                        if (object_step_count(arm) != now - start)
                                arch_puts("  note: one tick fewer than ticks elapsed, because the\n"
                                          "        first sighting only schedules (no free step),\n"
                                          "        and a late run does not catch up (decision 16)\n");
                        arch_puts("\n[live] objects keep moving; the base stays idle\n");
                        reported = true;
                }
        }
}
