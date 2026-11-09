/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * trajectory.c — 平面圆轨迹
 */
#include "trajectory.h"

bool path_init_circle(circular_path_t *p, motion_fixed_t cx, motion_fixed_t cy,
                      motion_fixed_t radius, motion_fixed_t omega)
{
        if (p == 0)
                return false;
        if (radius <= 0 || omega == 0)
                return false;

        p->cx = cx;
        p->cy = cy;
        p->radius = radius;
        p->omega = omega;
        p->phase = 0;
        p->last_ns = 0;
        p->steps = 0;
        return true;
}

bool path_step_circle(circular_path_t *p, u64 now_ns,
                      motion_fixed_t *x, motion_fixed_t *y)
{
        u64 dt_ns;
        motion_fixed_t dt_s, advance, s, c, ox, oy;

        if (p == 0 || x == 0 || y == 0)
                return false;

        /* 第一次只记时刻 */
        if (p->last_ns == 0) {
                p->last_ns = now_ns;
                return false;
        }
        if (now_ns < p->last_ns)
                return false;           /* 时间回退：不推进相位 */

        dt_ns = now_ns - p->last_ns;
        if (dt_ns == 0)
                return false;
        p->last_ns = now_ns;

        /* Δφ = ω · Δt，Δt 换算成秒（Q32.32） */
        dt_s = (motion_fixed_t)((u128)dt_ns << 32) / (motion_fixed_t)1000000000;
        if (fixed_mul_checked(p->omega, dt_s, &advance) != FIXED_OK)
                return false;
        if (fixed_add_checked(p->phase, advance, &p->phase) != FIXED_OK)
                return false;

        if (fixed_sin_cos(p->phase, &s, &c) != FIXED_OK)
                return false;

        /* 偏移量 = r · (cos φ, sin φ) */
        if (fixed_mul_checked(p->radius, c, &ox) != FIXED_OK ||
            fixed_mul_checked(p->radius, s, &oy) != FIXED_OK)
                return false;

        if (fixed_add_checked(p->cx, ox, x) != FIXED_OK ||
            fixed_add_checked(p->cy, oy, y) != FIXED_OK)
                return false;

        p->steps++;
        return true;
}
