/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * monitor.c — 基座之外的安全监视（只观察，不干预）
 *
 * 本文件不得调用 base_apply、object_set_state、object_describe。
 * 这不是自我约束，而是被 monitor_verify() 的前后比对证明的：
 * 轮询一次之后，每个物件的状态逐字段不变。
 *
 * 它自己的存储是允许的 —— 它在基座之外。「无状态」这条纪律
 * 约束的是基座，不是基座之上的观察者。
 */
#include "monitor.h"
#include "fixed.h"
#include "object.h"

#define MONITOR_MAX 32

typedef struct {
        wire_t          wire;
        motion_fixed_t  drift_limit;
        bool            in_use;
} monitor_entry_t;

static monitor_entry_t entries[MONITOR_MAX];
static monitor_sink_t  sink;
static motion_fixed_t  default_drift_limit;

void monitor_init(monitor_sink_t s)
{
        sink = s;
        default_drift_limit = 0;        /* 0 表示不判偏差 */

        for (u32 i = 0; i < MONITOR_MAX; i++) {
                entries[i].in_use = false;
                entries[i].wire = WIRE_NONE;
                entries[i].drift_limit = 0;
        }
}

static monitor_entry_t *find(wire_t w)
{
        for (u32 i = 0; i < MONITOR_MAX; i++)
                if (entries[i].in_use && entries[i].wire == w)
                        return &entries[i];
        return 0;
}

bool monitor_watch(wire_t w)
{
        if (w == WIRE_NONE || find(w) != 0)
                return false;

        for (u32 i = 0; i < MONITOR_MAX; i++) {
                if (entries[i].in_use)
                        continue;
                entries[i].wire = w;
                entries[i].drift_limit = default_drift_limit;
                entries[i].in_use = true;
                return true;
        }
        return false;
}

void monitor_forget(wire_t w)
{
        monitor_entry_t *e = find(w);
        if (e != 0) {
                e->in_use = false;
                e->wire = WIRE_NONE;
        }
}

void monitor_set_drift_limit(motion_fixed_t limit)
{
        default_drift_limit = limit;
}

static void report(monitor_event_t ev, wire_t w, u32 *count)
{
        (*count)++;
        if (sink != 0)
                sink(ev, w);
}

/* |a - b|，失败时返回「无穷大」的替代：直接报不相同 */
static bool abs_diff(motion_fixed_t a, motion_fixed_t b, motion_fixed_t *out)
{
        motion_fixed_t d;

        if (fixed_sub_checked(a, b, &d) != FIXED_OK)
                return false;
        if (d == INT64_MIN)
                return false;
        *out = d < 0 ? -d : d;
        return true;
}

monitor_stats_t monitor_poll(object_run_status_t run_status)
{
        monitor_stats_t st;
        u32 events = 0;

        st.watched = 0;
        st.alive = 0;
        st.events = 0;

        /* The caller advances execution; the monitor only observes its result. */
        if (run_status != OBJECT_RUN_OK)
                report(MON_E_CLOCK, 0, &events);

        for (u32 i = 0; i < MONITOR_MAX; i++) {
                monitor_entry_t *e = &entries[i];

                if (!e->in_use)
                        continue;
                st.watched++;

                /*
                 * 1. 还在不在。离开的接线用 monitor_forget() 正常注销，
                 *    所以这里报出来的一定是**非预期消失**。
                 *
                 *    报一次就注销：世代号保证消失的接线永远指向不了
                 *    任何东西，它不可能复活。若留着条目，每一次轮询
                 *    都会重复报同一起故障，把后面的现场淹掉 ——
                 *    而那正是「监视器吵到没人再听」的起点。
                 */
                if (!base_wire_valid(e->wire)) {
                        report(MON_E_WIRE_LOST, e->wire, &events);
                        e->in_use = false;
                        e->wire = WIRE_NONE;
                        continue;
                }
                st.alive++;

                /*
                 * 2. 世界解算还成不成立。断链、成环、链过深、溢出
                 *    都只影响解算，不影响物件自己继续动 ——
                 *    所以它们必须被单独看见，而不是等撞上才暴露。
                 */
                motion_fixed_t world[MOTION_DOF_MAX];
                object_resolve_status_t rs = object_resolve(e->wire, world);
                if (rs != OBJECT_RESOLVE_OK) {
                        report(MON_E_RESOLVE, e->wire, &events);
                        continue;
                }

                /*
                 * 3. 实测与命令的偏差。
                 *
                 *    参考实现里物件的控制环就是它自己，所以正常情况下
                 *    这两个值相等；一旦不等，说明物件的执行落后于命令
                 *    （堵转、丢步、驱动没跟上）。这个判断只产生事件。
                 */
                if (e->drift_limit > 0) {
                        object_state_t s;

                        if (object_state(e->wire, &s)) {
                                motion_fixed_t d;

                                if (!abs_diff(s.linear[0], object_motion(e->wire, 0), &d) ||
                                    d > e->drift_limit)
                                        report(MON_E_DRIFT, e->wire, &events);
                        }
                }

                /*
                 * 4. 实测速度是否越过物件自报的上限。
                 *
                 *    上限在物件侧拦的是**命令**；这里看的是**实测**。
                 *    两者不是一回事：命令被拦住了，物件仍可能因为
                 *    惯性、外力或驱动缺陷跑得更快。
                 */
                object_capability_t cap;

                if (object_capability(e->wire, &cap) && cap.dof > 0) {
                        object_state_t s;

                        if (object_state(e->wire, &s)) {
                                motion_fixed_t rate = s.linear[0];
                                motion_fixed_t limit = cap.max_rate[0];

                                /* Compare without negating INT64_MIN. */
                                if (limit > 0 &&
                                    (rate > limit || rate < -limit))
                                        report(MON_E_RATE, e->wire, &events);
                        }
                }
        }

        st.events = events;
        return st;
}
