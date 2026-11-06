/*
 * test_monitor.c — monitor host-side boundary tests
 *
 * Runs the real base/object/fixed/monitor implementations. The monitor must
 * observe the execution result without advancing the object clock or changing
 * object state. It also must handle INT64_MIN measured velocity without signed
 * negation or undefined behaviour.
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/object.c"
#include "../../monitor/monitor.c"

#include <stdint.h>
#include <stdio.h>

#define Q(v) ((motion_fixed_t)(v) * 4294967296LL)

static unsigned failures;
static unsigned seen[8];

static void check(const char *what, int ok)
{
        printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
                failures++;
}

static void record_event(monitor_event_t ev, wire_t wire)
{
        (void)wire;
        if (ev <= 0 && ev > -8)
                seen[-ev]++;
}

static void clear_seen(void)
{
        for (unsigned i = 0; i < 8; i++)
                seen[i] = 0;
}

static int same_state(const object_state_t *a, const object_state_t *b)
{
        for (unsigned i = 0; i < 3; i++) {
                if (a->position[i] != b->position[i] ||
                    a->linear[i] != b->linear[i] ||
                    a->angular[i] != b->angular[i])
                        return 0;
        }
        for (unsigned i = 0; i < 4; i++)
                if (a->orientation[i] != b->orientation[i])
                        return 0;
        return 1;
}

int main(void)
{
        printf("\n=== monitor host boundaries ===\n");

        base_init();
        object_init();
        monitor_init(record_event);
        monitor_set_drift_limit(Q(1));

        wire_t w = object_plug(2, 1000000ULL);
        check("test object plugs", w != WIRE_NONE);
        check("monitor accepts a live wire", monitor_watch(w));

        motion_event_t ev = {
                .wire = w,
                .frame = FRAME_WORLD,
                .issued_at_ns = 0,
                .duration_ns = 1000000000ULL,
                .dof = 1,
                .motion = { Q(1), 0, 0, 0, 0, 0 },
        };
        check("motion command is accepted", base_apply(0, &ev) == MOTION_OK);

        u32 stepped = 0;
        object_run_status_t run = object_run(1000000ULL, &stepped);
        object_state_t before, after;
        check("state readable before poll", object_state(w, &before));
        u64 clock_before = object_now_ns();
        u32 steps_before = object_step_count(w);

        clear_seen();
        monitor_stats_t stats = monitor_poll(run);
        check("healthy scene has no events", stats.events == 0);
        check("state readable after poll", object_state(w, &after));
        check("poll preserves object state", same_state(&before, &after));
        check("poll preserves execution clock", object_now_ns() == clock_before);
        check("poll preserves step count", object_step_count(w) == steps_before);

        object_capability_t cap = {0};
        cap.dof = 1;
        cap.limit_min[0] = INT64_MIN;
        cap.limit_max[0] = INT64_MAX;
        cap.max_rate[0] = Q(1);
        check("capability is accepted", object_describe(w, &cap));

        object_state_t extreme = after;
        extreme.linear[0] = INT64_MIN;
        check("INT64_MIN measured state is accepted", object_set_state(w, &extreme));
        clear_seen();
        monitor_poll(OBJECT_RUN_OK);
        check("INT64_MIN rate is reported without negation",
              seen[-MON_E_RATE] == 1);

        clear_seen();
        monitor_poll(OBJECT_RUN_E_CLOCK_ROLLBACK);
        check("caller clock failure is observed", seen[-MON_E_CLOCK] == 1);

        object_unplug(w);
        clear_seen();
        stats = monitor_poll(OBJECT_RUN_OK);
        check("unexpected departure is reported once",
              seen[-MON_E_WIRE_LOST] == 1 && stats.events == 1);
        clear_seen();
        monitor_poll(OBJECT_RUN_OK);
        check("lost wire is deregistered after one report",
              seen[-MON_E_WIRE_LOST] == 0);

        printf("\n  monitor host failures = %u\n", failures);
        return failures ? 1 : 0;
}
