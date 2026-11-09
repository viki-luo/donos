/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kinematics.c — 平面二连杆正运动学
 */
#include "kinematics.h"

static bool geom_valid(const leg_geometry_t *g)
{
        return g != 0 && g->l1 > 0 && g->l2 > 0;
}

bool leg_radius_squared(motion_fixed_t x, motion_fixed_t y,
                        motion_fixed_t *out)
{
        motion_fixed_t x2, y2;

        if (out == 0)
                return false;
        if (fixed_mul_checked(x, x, &x2) != FIXED_OK)
                return false;
        if (fixed_mul_checked(y, y, &y2) != FIXED_OK)
                return false;

        return fixed_add_checked(x2, y2, out) == FIXED_OK;
}

bool leg_tip(const leg_geometry_t *g, motion_fixed_t theta1,
             motion_fixed_t theta2, motion_fixed_t *x, motion_fixed_t *y)
{
        motion_fixed_t s1, c1, s12, c12, t12, ax, ay;

        if (!geom_valid(g) || x == 0 || y == 0)
                return false;

        if (fixed_sin_cos(theta1, &s1, &c1) != FIXED_OK)
                return false;

        /* θ1 + θ2 可能超出 2π，fixed_sin_cos 自带归约 */
        if (fixed_add_checked(theta1, theta2, &t12) != FIXED_OK)
                return false;
        if (fixed_sin_cos(t12, &s12, &c12) != FIXED_OK)
                return false;

        /* x = l1·cosθ1 + l2·cos(θ1+θ2) */
        if (fixed_mul_checked(g->l1, c1, &ax) != FIXED_OK)
                return false;
        if (fixed_mul_checked(g->l2, c12, &t12) != FIXED_OK)
                return false;
        if (fixed_add_checked(ax, t12, &ax) != FIXED_OK)
                return false;

        /* y = l1·sinθ1 + l2·sin(θ1+θ2) */
        if (fixed_mul_checked(g->l1, s1, &ay) != FIXED_OK)
                return false;
        if (fixed_mul_checked(g->l2, s12, &t12) != FIXED_OK)
                return false;
        if (fixed_add_checked(ay, t12, &ay) != FIXED_OK)
                return false;

        *x = ax;
        *y = ay;
        return true;
}

bool leg_reachable(const leg_geometry_t *g, motion_fixed_t x, motion_fixed_t y)
{
        motion_fixed_t r2, lo, hi, lo2, hi2;

        if (!geom_valid(g))
                return false;

        /* 外半径 l1 + l2 */
        if (fixed_add_checked(g->l1, g->l2, &hi) != FIXED_OK)
                return false;
        if (fixed_mul_checked(hi, hi, &hi2) != FIXED_OK)
                return false;

        /* 内半径 |l1 − l2| */
        if (fixed_sub_checked(g->l1, g->l2, &lo) != FIXED_OK)
                return false;
        if (lo < 0)
                lo = -(lo + 1) + 1;
        if (fixed_mul_checked(lo, lo, &lo2) != FIXED_OK)
                return false;

        if (!leg_radius_squared(x, y, &r2))
                return false;

        /*
         * 留一点容差：φ 表与 CORDIC 都有几个最低位的误差，
         * 恰好落在边界上的点会被误判为不可达。容差取外半径的 0.5%。
         */
        return r2 <= hi2 + hi2 / 200 && r2 + lo2 / 200 >= lo2;
}
