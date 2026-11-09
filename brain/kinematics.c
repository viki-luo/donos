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

/* acos(c) = atan2(√(1−c²), c)，全部定点 */
static leg_ik_status_t fixed_acos(motion_fixed_t c, motion_fixed_t *out)
{
        motion_fixed_t one, sq, root;

        if (out == 0)
                return LEG_IK_E_INVAL;

        /* 容差外才判为不可达：CORDIC 与开方都有几个最低位的误差 */
        if (c > Q_ONE + Q_ONE / 10000 || c < -Q_ONE - Q_ONE / 10000)
                return LEG_IK_E_UNREACHABLE;

        if (c > Q_ONE)
                c = Q_ONE;
        if (c < -Q_ONE)
                c = -Q_ONE;

        one = (motion_fixed_t)1 << 32;
        if (fixed_mul_checked(c, c, &sq) != FIXED_OK)
                return LEG_IK_E_INVAL;
        if (fixed_sub_checked(one, sq, &sq) != FIXED_OK)
                return LEG_IK_E_INVAL;
        if (fixed_sqrt(sq, &root) != FIXED_OK)
                return LEG_IK_E_INVAL;

        return fixed_atan2(root, c, out) == FIXED_OK ? LEG_IK_OK : LEG_IK_E_INVAL;
}

leg_ik_status_t leg_ik(const leg_geometry_t *g, motion_fixed_t x,
                       motion_fixed_t y, bool knee_up,
                       motion_fixed_t *theta1, motion_fixed_t *theta2)
{
        motion_fixed_t r2, l1sq, l2sq, num, den, cos_t2, t2, s2, c2;
        motion_fixed_t base, k1, k2, off, t1;

        if (!geom_valid(g) || theta1 == 0 || theta2 == 0)
                return LEG_IK_E_INVAL;

        if (!leg_radius_squared(x, y, &r2))
                return LEG_IK_E_INVAL;

        /* 足端在髋点：θ₁ 自由，解无穷多（且 atan2(0,0) 无定义） */
        if (r2 == 0)
                return LEG_IK_E_DEGENERATE;

        /* cos θ₂ = (r² − l1² − l2²) / (2·l1·l2) */
        if (fixed_mul_checked(g->l1, g->l1, &l1sq) != FIXED_OK ||
            fixed_mul_checked(g->l2, g->l2, &l2sq) != FIXED_OK)
                return LEG_IK_E_INVAL;
        if (fixed_sub_checked(r2, l1sq, &num) != FIXED_OK ||
            fixed_sub_checked(num, l2sq, &num) != FIXED_OK)
                return LEG_IK_E_INVAL;

        if (fixed_mul_checked(g->l1, g->l2, &den) != FIXED_OK ||
            fixed_mul2_checked(den, &den) != FIXED_OK)
                return LEG_IK_E_INVAL;
        if (den <= 0)
                return LEG_IK_E_INVAL;

        if (fixed_div_checked(num, den, &cos_t2) != FIXED_OK)
                return LEG_IK_E_INVAL;

        leg_ik_status_t st = fixed_acos(cos_t2, &t2);

        if (st != LEG_IK_OK)
                return st;

        /* 两个解：膝上取 +θ₂，膝下取 −θ₂ */
        if (!knee_up)
                t2 = -t2;

        if (fixed_sin_cos(t2, &s2, &c2) != FIXED_OK)
                return LEG_IK_E_INVAL;

        /*
         * θ₁ = atan2(y, x) − atan2(l2·sin θ₂, l1 + l2·cos θ₂)
         */
        if (fixed_atan2(y, x, &base) != FIXED_OK)
                return LEG_IK_E_INVAL;

        if (fixed_mul_checked(g->l2, s2, &k1) != FIXED_OK ||
            fixed_mul_checked(g->l2, c2, &k2) != FIXED_OK ||
            fixed_add_checked(g->l1, k2, &k2) != FIXED_OK)
                return LEG_IK_E_INVAL;
        if (fixed_atan2(k1, k2, &off) != FIXED_OK)
                return LEG_IK_E_INVAL;

        if (fixed_sub_checked(base, off, &t1) != FIXED_OK)
                return LEG_IK_E_INVAL;

        *theta1 = t1;
        *theta2 = t2;
        return LEG_IK_OK;
}
