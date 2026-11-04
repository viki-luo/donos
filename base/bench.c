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
#include "object.h"
#include "report.h"
#include "bench.h"

#define APPLY_N      200000u
#define RESOLVE_N     50000u
#define TICK_N         2000u
#define CHURN_N       20000u

#define CHAIN_DEPTH       7u    /* 7 hops, just under the depth limit of 8 */
#define CHAIN_COUNT       8u    /* 8 chains x 8 objects = 64, the whole table */

static int bench_failures;

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

        u64 t0 = arch_cycles();
        for (u32 i = 0; i < APPLY_N; i++)
                base_apply(clk, &ev);
        u64 accepted = arch_cycles() - t0;

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

static u32 bench_worst_tick(void)
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
        u64 t0 = arch_cycles();

        for (u32 i = 0; i < TICK_N; i++) {
                now += 10000000ULL;
                object_run(now, &stepped);
        }
        u64 cyc = (arch_cycles() - t0) / TICK_N;

        /*
         * The first run of a newly built set only schedules, so exactly one
         * of TICK_N runs is a scheduling pass rather than an integration
         * pass. Say so instead of letting the average imply 2000 full runs.
         */
        arch_puts("  (of these runs, 1 is the first sighting: schedule only)\n");

        arch_puts("  full run over ");
        arch_putdec(placed);
        arch_puts(" objects (every free slot) on ");
        arch_putdec(built);
        arch_puts(" turned chains : ");
        arch_putdec(cyc);
        arch_puts(" counter ticks/run\n");

        /*
         * Release the whole chain here, not just the bookkeeping: this
         * section deliberately fills every free object slot, and leaving
         * them plugged would make the stress section below measure a table
         * that is already full.
         */
        release_tracked();

        return (u32)cyc;
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
        u32 tick_cost = bench_worst_tick();

        u64 per_tick = bench_cycles_per_tick();
        arch_puts("  one 100 Hz tick period : ");
        arch_putdec(per_tick);
        arch_puts(" counter ticks\n");

        if (per_tick != 0u && tick_cost != 0u) {
                arch_puts("  worst run share of a tick : ");
                put_pct(tick_cost, per_tick);
                arch_puts("\n");
        }

        bench_stress();

        arch_puts("\n  bench failures = ");
        arch_putdec(bench_failures);
        arch_puts("\n");

        return bench_failures;
}

