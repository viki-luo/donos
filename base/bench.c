/*
 * bench.c - measured cost of the single primitive and of the execution
 * layer, plus the base's behaviour under stress.
 *
 * Three measurement rules, so that no number here is flattering but hollow:
 *
 *   1. Everything is measured on the target, not derived.
 *   2. Only same-counter ratios are reported as conclusions. "What
 *      fraction of a tick period does one full run take" is independent of
 *      the counter's absolute scale and therefore survives an emulator.
 *      "Nanoseconds per call" would not, so it is not claimed.
 *   3. The divisor of every division is a compile-time constant.
 *      Freestanding builds have no 64-bit division runtime; a variable
 *      divisor becomes __udivdi3 and fails at link time.
 */
#include "base.h"
#include "fixed.h"
#include "monitor.h"
#include "object.h"
#include "report.h"
#include "bench.h"

#define APPLY_N     200000u
#define RESOLVE_N    50000u
#define TICK_N         2000u
#define CHURN_N       20000u
#define LATENCY_HIST_BINS 64u

/* 长期运行与中断风暴的规模与期望值来源（额定节拍见 report.h）。 */
#define ENDURANCE_TICKS   1000u
#define ENDURANCE_STEP_NS (1000000000ULL / ARCH_TICK_HZ)
#define STORM_HZ          8000u
#define STORM_TICKS        500u
/*
 * 风暴突发允许耗掉「多少个额定节拍当量」的计数。
 * 8000 Hz 的理论值是 500/80 = 6.25；给到 50 仍是额定频率下 500 的 1/10，
 * 因此这条阈值不会把「频率没升」误判为通过。
 */
#define STORM_NOMINAL_EQUIV 50u

/* Q32.32 的整数量：BQ(3) = 3.0。 */
#define BQ(x) ((motion_fixed_t)(x) << 32)

/*
 * 100 Hz 满表推进的验收预算。
 *
 * p99.9 必须低于节拍的 50%，任何单次最大值必须低于 75%。
 * 自动门禁在 QEMU -icount 模式下执行，以排除宿主调度暂停并形成可重复
 * 的指令级回归基线。真实硬件仍必须复跑相同分位数，不能把 icount
 * 结果当成真实芯片的纳秒或 WCET 证明。
 */
#define LATENCY_P999_BUDGET_PERMILLE 500u
#define LATENCY_MAX_BUDGET_PERMILLE  750u

#define CHAIN_DEPTH       7u    /* 7 hops, just under the depth limit of 8 */
#define CHAIN_COUNT       8u    /* 8 chains x 8 objects = 64, the whole table */

static int bench_failures;

/* 满表 object_run 的逐次样本与精确尾部分位数。放在静态区，避免压栈。 */
static u64 tick_samples[TICK_N];
static u32 tick_hist[LATENCY_HIST_BINS];
static u64 tick_avg;
static u64 tick_p50;
static u64 tick_p99;
static u64 tick_p999;
static u64 tick_max;

/*
 * Submission instant used by the benchmark harness.
 *
 * Every event built here carries a full-width freshness window
 * (issued_at_ns = 0, duration_ns = UINT64_MAX), so what is being measured is
 * the mechanism's cost and never the freshness decision. The instant is a
 * fixed literal rather than whatever the execution clock happens to hold, so
 * a measurement cannot silently turn into a rejection.
 */
static u64 clk;

/*
 * Every wire a benchmark creates is recorded here and released when that
 * section ends. Without this, an earlier section would leave the object
 * table full and the next one would measure a table-full rejection
 * instead of what it meant to measure.
 */
#define TRACK_MAX 160u

static wire_t tracked[TRACK_MAX];
static u32    tracked_n;

static wire_t plug_tracked(u32 unit, u64 period)
{
        wire_t w = object_plug(unit, period);

        if (w != WIRE_NONE && tracked_n < TRACK_MAX)
                tracked[tracked_n++] = w;

        return w;
}

static void release_tracked(void)
{
        while (tracked_n > 0u)
                object_unplug(tracked[--tracked_n]);

        tracked_n = 0;
}

/*
 * How many object slots are free right now.
 *
 * This is a diagnostic, not part of the base: it plugs until the table
 * refuses, counts, and releases everything again. It matters because PCI
 * discovery and the semantic self-check run before the benchmarks, and if
 * either of them leaks a wire the capacity available to real objects is
 * silently reduced. Measuring the number makes such a leak visible instead
 * of turning it into a mystery in a later section.
 */
static u32 count_free_slots(void)
{
        release_tracked();

        while (tracked_n < TRACK_MAX) {
                wire_t w = object_plug(0, 0);
                if (w == WIRE_NONE)
                        break;
                tracked[tracked_n++] = w;
        }

        u32 free_now = tracked_n;
        release_tracked();

        return free_now;
}

static void check(const char *what, bool ok)
{
        arch_puts(ok ? "  ok   " : "  FAIL ");
        arch_puts(what);
        arch_puts("\n");
        if (!ok)
                bench_failures++;
}

/* Print a/b as a percentage with two decimals, without a variable divisor. */
static void put_pct(u64 a, u64 b)
{
        if (b == 0) {
                arch_puts("n/a");
                return;
        }

        /*
         * Work in hundredths of a percent: t = floor(10000 * a / b).
         * The integer percent is then t / 100 and the two decimals t % 100.
         *
         * Scaling by 10000 rather than 100 matters: with 100 the quotient is
         * the integer percent (1, not 1.37), so the fraction would be lost and
         * a 1.37% share would print as "0.01%".
         *
         * The division is done by repeated subtraction, so no variable divisor
         * ever reaches the code generator.
         */
        u64 scaled = 0;
        for (u32 i = 0; i < 10000u; i++)
                scaled += a;

        u32 t = 0;
        while (scaled >= b && t < 1000000u) {
                scaled -= b;
                t++;
        }

        arch_putdec(t / 100u);
        arch_puts(".");
        u32 frac = t % 100u;
        if (frac < 10u)
                arch_puts("0");
        arch_putdec(frac);
        arch_puts("%");
}

/*
 * Print total/N with two decimals.
 *
 * This is a macro rather than a function on purpose: a function would take N
 * as a variable, and a variable divisor compiles to __udivdi3, which does not
 * exist in a freestanding build. Substituting N textually keeps every divisor
 * a compile-time constant, so the division stays a multiply-and-shift.
 */
#define PUT_AVG(total, n)                                                \
        do {                                                             \
                arch_putdec((u64)(total) / (n));                         \
                arch_puts(".");                                          \
                u32 frac_ = (u32)(((u64)(total) % (n)) / ((n) / 100u));  \
                if (frac_ < 10u)                                         \
                        arch_puts("0");                                  \
                arch_putdec(frac_);                                      \
        } while (0)

static motion_event_t make_event(wire_t wire, wire_t frame, u64 duration,
                                 u8 dof, motion_fixed_t m0)
{
        motion_event_t ev;
        ev.wire = wire;
        ev.frame = frame;
        /*
         * 测量的是机制代价，不是新鲜度语义：发出时刻取 0、窗口取最大值，
         * 让每个事件无论合成时钟走到哪里都保持新鲜。
         * duration 参数仅保留以兼容调用点。
         */
        (void)duration;
        ev.issued_at_ns = 0;
        ev.duration_ns = UINT64_MAX;
        ev.dof = dof;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = 0;
        ev.motion[0] = m0;
        return ev;
}

/* Give the object a 90-degree turn about z, so resolution must rotate. */
static void set_turned(wire_t w)
{
        object_state_t st;
        if (!object_state(w, &st))
                return;
        st.orientation[0] = 3037000500LL;       /* cos 45 deg, Q32.32 */
        st.orientation[1] = 0;
        st.orientation[2] = 0;
        st.orientation[3] = 3037000500LL;       /* sin 45 deg */
        object_set_state(w, &st);
}

/* ------------------------------------------------------------------ */
/* 1. Cost of the single primitive                                      */
/* ------------------------------------------------------------------ */

static void bench_apply(void)
{
        wire_t w = plug_tracked(2, 0);          /* no period: not scheduled */
        motion_event_t ev = make_event(w, FRAME_WORLD, 1000000000ULL, 3, 1);

        arch_irq_disable();
        u64 t0 = arch_cycles();
        for (u32 i = 0; i < APPLY_N; i++)
                base_apply(clk, &ev);
        u64 accepted = arch_cycles() - t0;
        arch_irq_enable();

        /* Rejection path: the target wire does not exist. */
        motion_event_t bad = make_event(0xDEADBEEFULL, FRAME_WORLD,
                                        1000000000ULL, 3, 1);
        t0 = arch_cycles();
        for (u32 i = 0; i < APPLY_N; i++)
                base_apply(clk, &bad);
        u64 rejected = arch_cycles() - t0;

        arch_puts("  base_apply accepted : ");
        PUT_AVG(accepted, APPLY_N);
        arch_puts(" counter ticks/call over ");
        arch_putdec(APPLY_N);
        arch_puts(" calls\n");

        arch_puts("  base_apply rejected : ");
        PUT_AVG(rejected, APPLY_N);
        arch_puts(" counter ticks/call\n");

        release_tracked();
}

/* ------------------------------------------------------------------ */
/* 2. Reference-chain resolution cost against chain depth               */
/* ------------------------------------------------------------------ */

static wire_t build_chain(u32 hops)
{
        wire_t prev = FRAME_WORLD;
        wire_t tail = WIRE_NONE;

        for (u32 i = 0; i <= hops; i++) {
                wire_t w = plug_tracked(2, 0);
                if (w == WIRE_NONE)
                        return WIRE_NONE;

                motion_event_t ev = make_event(w, prev, 1000000000ULL, 3, 1);
                base_apply(clk, &ev);
                set_turned(w);

                prev = w;
                tail = w;
        }
        return tail;
}

static void bench_resolve(void)
{
        motion_fixed_t out[MOTION_DOF_MAX];

        for (u32 hops = 0; hops <= CHAIN_DEPTH; hops += CHAIN_DEPTH) {
                wire_t tail = build_chain(hops);

                if (tail == WIRE_NONE) {
                        /* Released on the way out: nothing accumulates in the table. */
                        arch_puts("  (not enough free wires for this depth)\n");
                        release_tracked();
                        return;
                }

                u64 t0 = arch_cycles();
                for (u32 i = 0; i < RESOLVE_N; i++)
                        (void)object_resolve(tail, out);
                u64 cyc = arch_cycles() - t0;

                arch_puts("  object_resolve, chain of ");
                arch_putdec(hops + 1u);
                arch_puts(" objects (each turned): ");
                PUT_AVG(cyc, RESOLVE_N);
                arch_puts(" counter ticks/call\n");

                release_tracked();
        }
}

/* ------------------------------------------------------------------ */
/* 3. Worst-case tick: every object sits on the deepest chain           */
/* ------------------------------------------------------------------ */

static u32 latency_bucket(u64 value)
{
        u32 bucket = 0;

        while (value > 1u && bucket + 1u < LATENCY_HIST_BINS) {
                value >>= 1;
                bucket++;
        }
        return bucket;
}

static void heap_sift_down(u64 values[TICK_N], u32 root, u32 count)
{
        for (;;) {
                u32 child = root * 2u + 1u;

                if (child >= count)
                        return;
                if (child + 1u < count && values[child] < values[child + 1u])
                        child++;
                if (values[root] >= values[child])
                        return;

                u64 tmp = values[root];
                values[root] = values[child];
                values[child] = tmp;
                root = child;
        }
}

static void sort_tick_samples(void)
{
        for (u32 i = TICK_N / 2u; i > 0u; i--)
                heap_sift_down(tick_samples, i - 1u, TICK_N);

        for (u32 end = TICK_N; end > 1u; end--) {
                u64 tmp = tick_samples[0];
                tick_samples[0] = tick_samples[end - 1u];
                tick_samples[end - 1u] = tmp;
                heap_sift_down(tick_samples, 0u, end - 1u);
        }
}

static void print_latency_histogram(void)
{
        arch_puts("  latency histogram (power-of-two counter ticks):\n");
        for (u32 i = 0; i < LATENCY_HIST_BINS; i++) {
                if (tick_hist[i] == 0u)
                        continue;
                arch_puts("    2^");
                arch_putdec(i);
                arch_puts(" bucket : ");
                arch_putdec(tick_hist[i]);
                arch_puts(" samples\n");
        }
}

static u64 bench_worst_tick(void)
{
    u32 built = 0;
    u32 placed = 0;
    bool full = false;

    for (u32 c = 0; c < CHAIN_COUNT && !full; c++) {
        wire_t prev = FRAME_WORLD;

        for (u32 d = 0; d <= CHAIN_DEPTH; d++) {
            wire_t w = plug_tracked(2, 10000000ULL);  /* 100 Hz */
            if (w == WIRE_NONE) {
                full = true;
                break;
            }
            placed++;
            motion_event_t ev = make_event(w, prev, 1000000000ULL, 6, 1);
            base_apply(clk, &ev);
            set_turned(w);
            prev = w;
        }
        built++;
    }

        if (placed == 0u) {
                release_tracked();
                arch_puts("  (no free object slot: nothing to measure)\n");
                return 0;
        }

        u64 now = 1000000000ULL;
        u32 stepped = 0;
        u64 total = 0;

        /* 首次只排期，不进入分位数样本。此后 TICK_N 次全部是完整积分。 */
        object_run(now, &stepped);
        for (u32 i = 0; i < LATENCY_HIST_BINS; i++)
                tick_hist[i] = 0;

        /*
         * 这里测的是 object_run 自身的执行成本，不把异步定时器中断和
         * 宿主 QEMU 调度暂停混进函数尾延迟。中断暴露的端到端延迟必须
         * 单独测量并单独给预算，不能用它冒充函数 WCET，也不能把异常
         * 离群值藏进平均数。关中断只包围逐次样本，测完立即恢复。
         */
        arch_irq_disable();
        for (u32 i = 0; i < TICK_N; i++) {
                now += 10000000ULL;
                u64 t0 = arch_cycles();
                object_run(now, &stepped);
                u64 elapsed = arch_cycles() - t0;

                tick_samples[i] = elapsed;
                tick_hist[latency_bucket(elapsed)]++;
                total += elapsed;
        }
        arch_irq_enable();

        tick_avg = total / TICK_N;
        sort_tick_samples();
        tick_p50 = tick_samples[(TICK_N * 500u) / 1000u - 1u];
        tick_p99 = tick_samples[(TICK_N * 990u) / 1000u - 1u];
        tick_p999 = tick_samples[(TICK_N * 999u) / 1000u - 1u];
        tick_max = tick_samples[TICK_N - 1u];

        arch_puts("  full run over ");
        arch_putdec(placed);
        arch_puts(" objects (every free slot) on ");
        arch_putdec(built);
        arch_puts(" turned chains\n");
        arch_puts("  latency average : "); arch_putdec(tick_avg); arch_puts(" counter ticks\n");
        arch_puts("  latency p50     : "); arch_putdec(tick_p50); arch_puts(" counter ticks\n");
        arch_puts("  latency p99     : "); arch_putdec(tick_p99); arch_puts(" counter ticks\n");
        arch_puts("  latency p99.9   : "); arch_putdec(tick_p999); arch_puts(" counter ticks\n");
        arch_puts("  latency max     : "); arch_putdec(tick_max); arch_puts(" counter ticks\n");
        print_latency_histogram();

        /*
         * Release the whole chain here, not just the bookkeeping: this
         * section deliberately fills every free object slot, and leaving
         * them plugged would make the stress section below measure a table
         * that is already full.
         */
        release_tracked();

        return tick_max;
}

/*
 * How many counter ticks one 100 Hz tick period is worth, measured with the
 * same counter. The ratio below therefore carries no assumption about the
 * counter's absolute frequency.
 */
#define TICK_SAMPLE 5u

static u64 bench_cycles_per_tick(void)
{
        u64 t_start = arch_ticks();
        u64 c_start = arch_cycles();

        while (arch_ticks() - t_start < TICK_SAMPLE)
                ;

        u64 dc = arch_cycles() - c_start;

        /* Divisor is the compile-time sample count, never a variable. */
        return dc / TICK_SAMPLE;
}

/* ------------------------------------------------------------------ */
/* 4. Stress: capacity, churn, forged handles                           */
/* ------------------------------------------------------------------ */

#define STRESS_MAX 128u

static void bench_stress(void)
{
        wire_t held[STRESS_MAX];
        u32 n = 0;

        arch_puts("\n=== stress: capacity, churn, forged handles ===\n\n");

        /*
         * Capacity is whatever this system happens to have free right now.
         * Bus discovery may legitimately keep devices plugged for the life of
         * the machine, so the expected number is not a constant and a check
         * against one would be wrong in either direction.
         */
        wire_t probe = object_plug(0, 0);
        if (probe == WIRE_NONE) {
                arch_puts("       capacity = 0 (no free slot); skipping the stress checks\n");
                return;
        }
        object_unplug(probe);

        /* Fill the wiring table until it refuses. */
        while (n < STRESS_MAX) {
                wire_t x = object_plug(0, 0);
                if (x == WIRE_NONE)
                        break;
                held[n++] = x;
        }

        u64 t0 = arch_cycles();
        wire_t overflow = object_plug(0, 0);
        u64 cost = arch_cycles() - t0;

        check("plugging past capacity fails closed (WIRE_NONE)",
              overflow == WIRE_NONE && n > 0u);

        /*
         * If nothing could be plugged at all, the checks below would index
         * held[n - 1] with n == 0, i.e. held[-1]. Stop here instead.
         */
        if (n == 0u) {
                arch_puts("       capacity = 0; skipping the remaining stress checks\n");
                return;
        }

        arch_puts("       capacity = ");
        arch_putdec(n);
        arch_puts(" wires; rejection cost ");
        arch_putdec(cost);
        arch_puts(" counter ticks (one failed scan)\n");

        /* Every wire that was already there must still work. */
        bool alive = true;
        for (u32 i = 0; i < n; i++) {
                motion_event_t ev = make_event(held[i], FRAME_WORLD, 1000ULL, 1, 1);
                if (base_apply(clk, &ev) != MOTION_OK)
                        alive = false;
        }
        check("a full table leaves existing wires intact", alive);

        /* Free one slot and take it again: the generation must move on. */
        wire_t victim = held[n - 1u];
        object_unplug(victim);
        wire_t fresh = object_plug(0, 0);

        check("a reused slot yields a different handle", fresh != victim);
        check("the departed handle stays dead", !base_wire_valid(victim));

        motion_event_t to_dead = make_event(victim, FRAME_WORLD, 1000ULL, 1, 1);
        check("the base rejects motion on a dead handle",
              base_apply(clk, &to_dead) == MOTION_E_NO_WIRE);

        /* Forged handles of every awkward shape must be refused, not crash. */
        wire_t forged[7];
        forged[0] = 0xFFFFFFFFFFFFFFFFULL;
        forged[1] = WIRE_NONE;
        forged[2] = 0x0000000000000001ULL;
        forged[3] = 0x00010040ULL;                       /* slot 64, gen 1 */
        forged[4] = 0x00010041ULL;                       /* slot 65, past the end */
        forged[5] = WIRE_MAKE(WIRE_GEN(fresh) + 1u, WIRE_INDEX(fresh));
        forged[6] = fresh | 0x8000000000000000ULL;       /* high-bit alias of a live handle */

        bool refused = true;
        for (u8 i = 0; i < 7u; i++) {
                motion_event_t ev = make_event(forged[i], FRAME_WORLD, 1000ULL, 1, 1);
                if (base_apply(clk, &ev) != MOTION_E_NO_WIRE)
                        refused = false;
                if (base_wire_valid(forged[i]))
                        refused = false;
        }
        check("seven forged handle shapes are all refused", refused);

        /* Churn: plug and unplug many times, watching the generation counter. */
        object_unplug(fresh);

        wire_t first = WIRE_NONE;
        wire_t prev = WIRE_NONE;
        u32 gen_moves = 0;
        u32 done = 0;

        for (u32 i = 0; i < CHURN_N; i++) {
                wire_t x = object_plug(0, 0);
                if (x == WIRE_NONE)
                        break;
                if (i == 0u)
                        first = x;
                if (prev != WIRE_NONE && WIRE_GEN(x) != WIRE_GEN(prev))
                        gen_moves++;
                prev = x;
                done++;
                object_unplug(x);
        }

        check("churn: generation advances on every reuse", gen_moves + 1u == done);
        check("churn: the first handle captured is long dead",
              first != WIRE_NONE && !base_wire_valid(first));

        arch_puts("       churn cycles = ");
        arch_putdec(done);
        arch_puts("\n");

        /* Release what is left, so later code sees a clean table. */
        for (u32 i = 0; i + 1u < n; i++)
                object_unplug(held[i]);
}

/* ------------------------------------------------------------------ */
/* 5. Endurance: 1000 scheduled ticks under continuous monitoring       */
/* ------------------------------------------------------------------ */

/*
 * 长期运行不是「跑得久」，而是「跑得久之后账还对得上」。1000 拍
 * = 10 s 执行层时间；每条期望都从余数进位的精确模型手算：
 *
 *   位置 = V·P·N / 1e9（Q32.32），当 V·P·N 是 1e9 的整数倍时精确成立，
 *   没有任何累计舍入。本节的三个速度都刻意选成这种情况：
 *
 *   run   : 3 m/s × 10 s  = 30 m（精确）—— 账必须分毫不差
 *   limit : 1 m/s，行程 [-1 m, +4 m] —— 第 400 拍精确停在 4 m，
 *           其后 600 拍整步拒绝；被拒的步仍计入执行
 *   spin  : 1 rad/s × 10 s，姿态每拍归一化 —— 归一化若有漂移，
 *           1000 步后 |q|² 一定离开 1
 *
 * 监视处置与长期运行同时进行：1000 次轮询，第 300 拍拔掉一条被关注的
 * 接线。故障必须恰好报一次（世代注销），其余轮询必须安静，
 * 幸存者必须一直被注视到最后一拍。
 */

static u32 ev_lost, ev_drift, ev_resolve, ev_rate, ev_clock;

static void endurance_sink(monitor_event_t ev, wire_t w)
{
        (void)w;

        switch (ev) {
        case MON_OK:           break;      /* 不是事件 */
        case MON_E_WIRE_LOST:  ev_lost++;    break;
        case MON_E_DRIFT:      ev_drift++;   break;
        case MON_E_RESOLVE:    ev_resolve++; break;
        case MON_E_RATE:       ev_rate++;    break;
        case MON_E_CLOCK:      ev_clock++;   break;
        }
}

static void bench_endurance(void)
{
        arch_puts("\n=== endurance: ");
        arch_putdec(ENDURANCE_TICKS);
        arch_puts(" scheduled ticks, monitored ===\n\n");

        wire_t run_obj   = plug_tracked(1, ENDURANCE_STEP_NS);
        wire_t spin_obj  = plug_tracked(1, ENDURANCE_STEP_NS);
        wire_t limit_obj = plug_tracked(1, ENDURANCE_STEP_NS);
        wire_t victim    = plug_tracked(1, ENDURANCE_STEP_NS);
        wire_t survivor  = plug_tracked(1, ENDURANCE_STEP_NS);

        if (survivor == WIRE_NONE) {
                check("endurance: five objects plugged", false);
                return;
        }

        motion_event_t ev = make_event(run_obj, FRAME_WORLD, 0, 1, BQ(3));
        check("endurance: the 3 m/s command is accepted",
              base_apply(clk, &ev) == MOTION_OK);

        ev = make_event(limit_obj, FRAME_WORLD, 0, 1, BQ(1));
        check("endurance: the 1 m/s command is accepted",
              base_apply(clk, &ev) == MOTION_OK);

        ev = make_event(victim, FRAME_WORLD, 0, 1, BQ(1));
        base_apply(clk, &ev);
        ev = make_event(survivor, FRAME_WORLD, 0, 1, BQ(1));
        base_apply(clk, &ev);

        ev = make_event(spin_obj, FRAME_WORLD, 0, 6, 0);
        ev.motion[5] = BQ(1);                 /* 角速度 z = 1 rad/s */
        check("endurance: the 1 rad/s spin is accepted",
              base_apply(clk, &ev) == MOTION_OK);

        object_capability_t cap;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++) {
                cap.limit_min[i] = 0;
                cap.limit_max[i] = 0;
                cap.max_rate[i] = 0;
        }
        cap.dof = 1;
        cap.mass = BQ(1);
        cap.limit_min[0] = -BQ(1);
        cap.limit_max[0] = BQ(4);
        cap.max_rate[0] = BQ(2);
        check("endurance: the travel limit is declared",
              object_describe(limit_obj, &cap));

        ev_lost = ev_drift = ev_resolve = ev_rate = ev_clock = 0;
        monitor_init(endurance_sink);
        check("endurance: the monitor watches the victim", monitor_watch(victim));
        check("endurance: the monitor watches the survivor",
              monitor_watch(survivor));

        /* 首次只排期：先空跑一次，让五条线全部进入调度。 */
        u64 t = object_now_ns();
        u32 stepped = 0;
        object_run(t, &stepped);

        u32 total_steps = 0;
        for (u32 k = 0; k < ENDURANCE_TICKS; k++) {
                t += ENDURANCE_STEP_NS;
                object_run_status_t rs = object_run(t, &stepped);
                total_steps += stepped;

                if (k == 300u)
                        object_unplug(victim);   /* 故意不调用 monitor_forget */

                monitor_poll(rs);
        }

        arch_puts("  endurance ticks = ");
        arch_putdec(ENDURANCE_TICKS);
        arch_puts(", steps = ");
        arch_putdec(total_steps);
        arch_puts("\n");

        check("endurance: the execution clock advanced exactly 10 s",
              object_now_ns() == t);
        check("endurance: every scheduled step executed",
              total_steps ==
              4u * ENDURANCE_TICKS + 301u); /* victim takes its last
                                               step in the same call that
                                               precedes its unplug */

        object_state_t st;
        check("endurance: 3 m/s for 10 s lands exactly on 30 m",
              object_state(run_obj, &st) && st.position[0] == BQ(30));
        check("endurance: the limited axis holds exactly at +4 m",
              object_state(limit_obj, &st) && st.position[0] == BQ(4));
        check("endurance: refused steps still count as executed",
              object_step_count(limit_obj) == ENDURANCE_TICKS);
        check("endurance: the spin never translates",
              object_state(spin_obj, &st) && st.position[0] == 0 &&
              st.position[1] == 0 && st.position[2] == 0);

        /* |q|² 仍须落在 1 ± 1/512：归一化若有漂移，1000 步后一定可见。 */
        bool unit = object_state(spin_obj, &st);
        motion_fixed_t sum = 0;
        for (u8 i = 0; i < 4 && unit; i++) {
                motion_fixed_t sq;

                unit = fixed_mul_checked(st.orientation[i], st.orientation[i],
                                         &sq) == FIXED_OK &&
                       fixed_add_checked(sum, sq, &sum) == FIXED_OK;
        }
        motion_fixed_t dev = 0;
        if (unit)
                unit = fixed_sub_checked(sum, (motion_fixed_t)1 << 32, &dev)
                       == FIXED_OK;
        if (unit && dev < 0)
                dev = -dev;
        check("endurance: the pose is still a unit quaternion after 1000 steps",
              unit && dev <= ((motion_fixed_t)1 << 32) / 512);
        check("endurance: the spin stayed about z only",
              object_state(spin_obj, &st) && st.orientation[1] == 0 &&
              st.orientation[2] == 0);

        monitor_stats_t ms = monitor_poll(OBJECT_RUN_OK);
        check("endurance: the departed wire was reported exactly once",
              ev_lost == 1u);
        check("endurance: no drift, resolve, rate or clock fault in 1000 polls",
              ev_drift == 0u && ev_resolve == 0u && ev_rate == 0u &&
              ev_clock == 0u);
        check("endurance: the monitor still watches the survivor to the end",
              ms.watched == 1u && ms.alive == 1u);

        monitor_forget(survivor);
}

/* ------------------------------------------------------------------ */
/* 6. Interrupt storm: an 8 kHz burst, then the nominal rate back       */
/* ------------------------------------------------------------------ */

/*
 * 风暴判据不是「收到很多中断」，而是三件可观察的事：
 *   1. 突发在有限的轮询次数内完成 —— 若频率仍是额定 100 Hz，500 拍
 *      要 5 s 虚拟时间，自旋次数会远远越出预算；这条把「真的升频了」
 *      变成可证伪的断言。
 *   2. 风暴结束后节拍继续前进 —— 没有因丢 EOI 或漏重装而停摆。
 *   3. 每拍周期数回到额定水平 —— 真的恢复了，而不是停在风暴频率。
 * 全部用同一计数器同一次运行内的比值判定，跨架构可比。
 */

static void bench_storm(void)
{
        arch_puts("\n=== interrupt storm: ");
        arch_putdec(STORM_HZ);
        arch_puts(" Hz burst, then nominal restore ===\n\n");

        u64 t0 = arch_ticks();
        u64 c0 = arch_cycles();
        while (arch_ticks() - t0 < 5u)
                ;
        u64 base_per_tick = (arch_cycles() - c0) / 5u;

        /*
         * 升频证据用「突发实际耗掉的计数」与「额定节拍宽度」相比。
         *
         * 若频率真的到 8000 Hz，500 拍只需 500/8000 秒，即额定 100 Hz 下
         * 500 拍耗时的 1/80。给足 8 倍余量（50 个额定节拍的当量），
         * 仍与「频率根本没变」（500 个额定节拍当量）相差 10 倍 ——
         * 这条判据无法被「中断只是变多」蒙混过去。
         */
        u64 s0 = arch_ticks();
        u64 sc0 = arch_cycles();
        arch_timer_hz(STORM_HZ);
        u64 spins = 0;
        while (arch_ticks() - s0 < STORM_TICKS)
                spins++;
        u64 storm_ticks = arch_ticks() - s0;
        u64 storm_cycles = arch_cycles() - sc0;
        arch_timer_hz(ARCH_TICK_HZ);

        arch_puts("  storm ticks = ");
        arch_putdec(storm_ticks);
        arch_puts(", spin iterations = ");
        arch_putdec(spins);
        arch_puts("\n");

        check("storm: the burst delivered its full tick count",
              storm_ticks >= STORM_TICKS);
        check("storm: the burst really ran far faster than the nominal rate",
              base_per_tick != 0u &&
              storm_cycles < base_per_tick * STORM_NOMINAL_EQUIV);

        u64 r0 = arch_ticks();
        u64 c1 = arch_cycles();
        while (arch_ticks() - r0 < 5u)
                ;
        u64 after_per_tick = (arch_cycles() - c1) / 5u;

        check("storm: the nominal tick rate is restored",
              after_per_tick >= base_per_tick / 4u &&
              after_per_tick <= base_per_tick * 4u + 4u);
        check("storm: ticks keep advancing after the burst",
              arch_ticks() > r0);

        u32 stepped = 0;
        check("storm: the execution layer still advances afterwards",
              object_run(object_now_ns() + ENDURANCE_STEP_NS, &stepped) ==
              OBJECT_RUN_OK);
}

/* ------------------------------------------------------------------ */

int bench_run(void)
{
        bench_failures = 0;

        arch_puts("\n=== measurement: cost of the single primitive ===\n\n");

        /*
         * Report the free capacity on entry. Discovery and self-check ran
         * before this point and may legitimately hold wires; if the number is
         * unexpectedly small, a leak rather than a design limit is the cause.
         */
        arch_puts("  free object slots on entry : ");
        arch_putdec(count_free_slots());
        arch_puts("\n\n");

        bench_apply();

        arch_puts("\n=== measurement: reference-chain resolution ===\n\n");
        bench_resolve();

        /* The sections above release every wire they took; the table is empty here. */
        arch_puts("\n=== measurement: worst-case tick ===\n\n");
        u64 tick_cost = bench_worst_tick();

        u64 per_tick = bench_cycles_per_tick();
        arch_puts("  one 100 Hz tick period : ");
        arch_putdec(per_tick);
        arch_puts(" counter ticks\n");

        if (per_tick != 0u && tick_cost != 0u) {
                arch_puts("  average run share of a tick : ");
                put_pct(tick_avg, per_tick);
                arch_puts("\n  p99.9 run share of a tick  : ");
                put_pct(tick_p999, per_tick);
                arch_puts("\n  max run share of a tick    : ");
                put_pct(tick_max, per_tick);
                arch_puts("\n");

                check("latency budget: p99.9 is below 50% of one tick",
                      tick_p999 <= (per_tick / 1000u) *
                                  LATENCY_P999_BUDGET_PERMILLE);
                check("latency budget: max is below 75% of one tick",
                      tick_max <= (per_tick / 1000u) *
                                 LATENCY_MAX_BUDGET_PERMILLE);
        }

        bench_endurance();
        bench_storm();
        bench_stress();

        arch_puts("\n  bench failures = ");
        arch_putdec(bench_failures);
        arch_puts("\n");

        return bench_failures;
}

