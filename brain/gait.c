/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gait.c — 相位驱动的关节目标角
 *
 * 目标角 = 中位角 + 摆幅 · sin(相位 + 偏移)
 *
 * 用正弦而不是查表：正弦在定点上是**连续**的，相位推进不均匀时轨迹也连续；
 * 查表会在步长不稳时出现台阶，而台阶对电机的含义是一次冲击。
 */
#include "gait.h"
#include "kinematics.h"

bool gait_init(gait_t *g, u64 period_ns, motion_fixed_t omega)
{
        if (g == 0)
                return false;
        if (period_ns == 0 || period_ns > 1000000000ULL)
                return false;
        if (omega == 0)
                return false;

        g->count = 0;
        g->period_ns = period_ns;
        g->omega = omega;
        g->phase = 0;
        g->last_ns = 0;
        g->steps = 0;
        return true;
}

bool gait_add_joint(gait_t *g, wire_t w, motion_fixed_t centre,
                    motion_fixed_t amplitude, motion_fixed_t phase)
{
        if (g == 0 || g->count >= GAIT_JOINTS_MAX)
                return false;
        if (!base_wire_valid(w))
                return false;
        if (amplitude < 0)
                return false;

        g->joints[g->count].wire = w;
        g->joints[g->count].centre = centre;
        g->joints[g->count].amplitude = amplitude;
        g->joints[g->count].phase = phase;
        g->count++;
        return true;
}

bool gait_step(gait_t *g, u64 now_ns, motion_fixed_t *targets,
               u8 targets_capacity)
{
        u64 dt_ns;
        motion_fixed_t dt_s, advance, s, c;

        if (g == 0 || targets == 0 || targets_capacity < g->count)
                return false;

        /*
         * 第一次调用只记时刻：相位从哪里开始是调用者的事，
         * 这里不能凭空假设「上次在哪」。
         */
        if (g->last_ns == 0) {
                g->last_ns = now_ns;
                return false;
        }

        if (now_ns < g->last_ns)
                return false;           /* 时间回退：不推进相位 */

        dt_ns = now_ns - g->last_ns;
        if (dt_ns == 0)
                return false;
        g->last_ns = now_ns;

        /* 相位推进：Δφ = ω · Δt，Δt 换算成秒（Q32.32） */
        dt_s = (motion_fixed_t)((u128)dt_ns << 32) / (motion_fixed_t)1000000000;
        if (fixed_mul_checked(g->omega, dt_s, &advance) != FIXED_OK)
                return false;
        if (fixed_add_checked(g->phase, advance, &g->phase) != FIXED_OK)
                return false;

        for (u8 i = 0; i < g->count; i++) {
                motion_fixed_t phi, swing;

                if (fixed_add_checked(g->phase, g->joints[i].phase, &phi) != FIXED_OK)
                        return false;
                if (fixed_sin_cos(phi, &s, &c) != FIXED_OK)
                        return false;
                if (fixed_mul_checked(g->joints[i].amplitude, s, &swing) != FIXED_OK)
                        return false;
                if (fixed_add_checked(g->joints[i].centre, swing,
                                      &targets[i]) != FIXED_OK)
                        return false;
        }

        g->steps++;
        return true;
}
