/*
 * monitor_verify.c — 监视器的确定性自检（基座之外）
 *
 * 三件事必须被证明，缺一条这个监视器就只是装饰：
 *   1. 该报的能报出来（每类事件各造一次现场）；
 *   2. 不该报的不报（健康现场必须零事件）；
 *   3. **它确实什么都没改** —— 轮询前后每个物件逐字段相同。
 * 第三条是「观察者」与「干预者」的分界，也是它能在基座之上
 * 而不破坏「基座无判断」的原因。
 */
#include "monitor.h"
#include "arch.h"
#include "object.h"

#define MILLI 1000000ULL
#define Q(v)  ((motion_fixed_t)(v) * 4294967296LL)

static int mfail;
static u32 seen_events;
static u32 seen_by_type[8];        /* 下标 = -event */

static void sink_count(monitor_event_t ev, wire_t w)
{
        seen_events++;
        if (ev <= 0 && ev > -8)
                seen_by_type[-ev]++;
        (void)w;
}

static void seen_reset(void)
{
        seen_events = 0;
        for (u32 i = 0; i < 8; i++)
                seen_by_type[i] = 0;
}

static bool seen_is(monitor_event_t ev)
{
        return ev <= 0 && ev > -8 && seen_by_type[-ev] > 0;
}

static void mcheck(const char *what, bool ok)
{
        arch_puts(ok ? "  ok   " : "  FAIL ");
        arch_puts(what);
        arch_puts("\n");
        if (!ok)
                mfail++;
}

static void make_event(motion_event_t *ev, wire_t w, wire_t frame,
                       u64 issued, u64 duration, u8 dof, motion_fixed_t v)
{
        ev->wire = w;
        ev->frame = frame;
        ev->issued_at_ns = issued;
        ev->duration_ns = duration;
        ev->dof = dof;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev->motion[i] = 0;
        ev->motion[0] = v;
}

/* 两次状态是否逐字段相同 —— 用来证明监视器没有改动任何东西 */
static bool same_state(const object_state_t *a, const object_state_t *b)
{
        for (u8 i = 0; i < 3; i++) {
                if (a->position[i] != b->position[i])
                        return false;
                if (a->linear[i] != b->linear[i])
                        return false;
                if (a->angular[i] != b->angular[i])
                        return false;
        }
        for (u8 i = 0; i < 4; i++)
                if (a->orientation[i] != b->orientation[i])
                        return false;
        return true;
}

int monitor_verify(void)
{
        mfail = 0;

        arch_puts("\n--- out-of-base monitor: observation without authority ---\n\n");

        monitor_init(sink_count);
        monitor_set_drift_limit(Q(1));

        u64 now = object_now_ns() + MILLI;
        u32 stepped = 0;

        /* ---- 健康现场：必须零事件 ---- */
        wire_t ok1 = object_plug(2, MILLI);
        make_event(&(motion_event_t){0}, ok1, FRAME_WORLD, now, 1000 * MILLI,
                   1, Q(1));
        motion_event_t ev;
        make_event(&ev, ok1, FRAME_WORLD, now, 1000 * MILLI, 1, Q(1));
        base_apply(now, &ev);

        monitor_watch(ok1);
        seen_reset();

        for (u32 k = 1; k <= 5; k++) {
                monitor_poll(now + k * MILLI, &stepped);

                /* 每次轮询后状态必须原样：监视器不是干预者 */
                object_state_t a, b;
                if (object_state(ok1, &a)) {
                        monitor_poll(now + k * MILLI + 1, &stepped);
                        if (object_state(ok1, &b) && !same_state(&a, &b)) {
                                mcheck("a poll does not change any object state",
                                       false);
                                goto next_section;
                        }
                }
        }

        mcheck("a healthy scene produces no events", seen_events == 0);
        mcheck("a poll does not change any object state", true);

next_section:
        /* ---- 非预期消失 ---- */
        seen_reset();
        {
                wire_t doomed = object_plug(2, MILLI);
                monitor_watch(doomed);
                object_unplug(doomed);          /* 没有调用 monitor_forget */
                monitor_poll(now + 10 * MILLI, &stepped);
                mcheck("an unregistered departure is reported as WIRE_LOST",
                       seen_events >= 1 && seen_is(MON_E_WIRE_LOST));
        }

        /* ---- 正常离开：不应被报成故障 ---- */
        seen_reset();
        {
                wire_t gone = object_plug(2, MILLI);
                monitor_watch(gone);
                monitor_forget(gone);           /* 先注销，再离开 */
                object_unplug(gone);
                monitor_poll(now + 11 * MILLI, &stepped);
                mcheck("a registered departure is not reported as a fault",
                       seen_events == 0);
        }

        /* ---- 解算失败：成环 ---- */
        seen_reset();
        {
                wire_t c1 = object_plug(2, MILLI);
                wire_t c2 = object_plug(2, MILLI);

                make_event(&ev, c1, c2, now, 1000 * MILLI, 1, 1);
                base_apply(now, &ev);
                make_event(&ev, c2, c1, now, 1000 * MILLI, 1, 1);
                base_apply(now, &ev);

                monitor_watch(c1);
                monitor_poll(now + 12 * MILLI, &stepped);
                mcheck("a cycle is reported as RESOLVE, not silently ignored",
                       seen_is(MON_E_RESOLVE));

                monitor_forget(c1);
                object_unplug(c1);
                object_unplug(c2);
        }

        /* ---- 实测超出自报上限 ---- */
        seen_reset();
        {
                wire_t fast = object_plug(2, MILLI);

                object_capability_t cap;
                cap.dof = 1;
                cap.mass = 0;
                for (u8 i = 0; i < MOTION_DOF_MAX; i++) {
                        cap.limit_min[i] = -Q(100);
                        cap.limit_max[i] = Q(100);
                        cap.max_rate[i] = Q(1);         /* 上限 1 m/s */
                }
                object_describe(fast, &cap);

                /* 直接把实测速度写成 5 m/s：命令没越界，实测越界 */
                object_state_t st;
                object_state(fast, &st);
                st.linear[0] = Q(5);
                object_set_state(fast, &st);

                monitor_watch(fast);
                monitor_poll(now + 13 * MILLI, &stepped);
                mcheck("a measured speed above the declared limit is reported",
                       seen_is(MON_E_RATE));

                monitor_forget(fast);
                object_unplug(fast);
        }

        /* ---- 执行层拒绝时刻 ---- */
        seen_reset();
        monitor_poll(0, &stepped);              /* 回退到 0：必被拒绝 */
        mcheck("a refused instant is reported as CLOCK",
               seen_is(MON_E_CLOCK));

        monitor_forget(ok1);
        object_unplug(ok1);

        arch_puts("\n  monitor failures = ");
        arch_putdec((u64)mfail);
        arch_puts("\n");

        return mfail;
}
