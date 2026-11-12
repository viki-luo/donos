/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * quadruped.c — 对角小跑步态
 */
#include "quadruped.h"

/*
 * 取小数部分：相位归一化到 [0,1)。
 *
 * 整数部分 = 原始值 ÷ 2^32（**整数**除法）。早先写成
 * fixed_div_checked(v, 1.0)，那算的是 Q(v/1.0) = v 本身，
 * 小数部分恒为 0 —— 相位永远不推进，四条腿全部停在支撑相。
 * 这不是逻辑疏漏的小错：整个步态因此「看起来在跑，实际上没在走」。
 * 除数是编译期常数，编译器用移位，不产生除法调用。
 */
static motion_fixed_t frac_q(motion_fixed_t v)
{
        const motion_fixed_t one = (motion_fixed_t)1 << 32;
        s64 whole = v / (s64)one;
        motion_fixed_t frac;

        /* 负数相位也要落在 [0,1)：先加一圈再取 */
        if (v < 0)
                whole -= 1;

        frac = v - (motion_fixed_t)whole * one;

        if (frac < 0)
                frac += one;
        if (frac >= one)
                frac -= one;

        return frac;
}

bool quad_init(quadruped_t *q, u64 period_ns, motion_fixed_t speed,
               motion_fixed_t duty, motion_fixed_t stride,
               motion_fixed_t lift, motion_fixed_t height)
{
        if (q == 0)
                return false;
        if (period_ns == 0 || period_ns > 1000000000ULL)
                return false;
        /* duty 必须是开区间 (0,1)：0 或 1 会让某一相永远不出现 */
        if (duty <= 0 || duty >= ((motion_fixed_t)1 << 32))
                return false;
        if (speed < 0 || stride < 0 || lift < 0 || height <= 0)
                return false;

        q->count = 0;
        q->period_ns = period_ns;
        q->speed = speed;
        q->duty = duty;
        q->stride = stride;
        q->lift = lift;
        q->height = height;
        q->phase = 0;
        q->body_x = 0;
        q->last_ns = 0;
        q->steps = 0;
        return true;
}

bool quad_add_leg(quadruped_t *q, wire_t hip, wire_t knee,
                  const leg_geometry_t *geom, motion_fixed_t phase,
                  motion_fixed_t x0, motion_fixed_t y0)
{
        if (q == 0 || q->count >= QUAD_LEGS_MAX || geom == 0)
                return false;
        if (!base_wire_valid(hip) || !base_wire_valid(knee))
                return false;

        quad_leg_t *leg = &q->legs[q->count];

        leg->hip = hip;
        leg->knee = knee;
        leg->geom = *geom;
        leg->phase = frac_q(phase);
        leg->x0 = x0;
        leg->y0 = y0;
        leg->installed = true;
        q->count++;
        return true;
}

bool quad_leg_in_stance(const quadruped_t *q, u8 leg)
{
        motion_fixed_t lp;

        if (q == 0 || leg >= q->count)
                return false;

        lp = frac_q(q->phase + q->legs[leg].phase);
        return lp < q->duty;
}

bool quad_step(quadruped_t *q, u64 now_ns,
               motion_fixed_t targets[QUAD_LEGS_MAX][2],
               bool in_stance[QUAD_LEGS_MAX])
{
        u64 dt_ns;
        motion_fixed_t dt_s, advance, dx;

        if (q == 0 || targets == 0 || in_stance == 0)
                return false;

        if (q->last_ns == 0) {
                q->last_ns = now_ns;
                return false;
        }
        if (now_ns < q->last_ns)
                return false;

        dt_ns = now_ns - q->last_ns;
        if (dt_ns == 0)
                return false;
        q->last_ns = now_ns;

        dt_s = (motion_fixed_t)((u128)dt_ns << 32) / (motion_fixed_t)1000000000;

        /* 相位推进：全局一圈 = 一个步态周期。周期由 stride/speed 决定。 */
        if (fixed_mul_checked(q->speed, dt_s, &dx) == FIXED_OK)
                fixed_add_checked(q->body_x, dx, &q->body_x);

        /*
         * 相位频率由**运动学一致性**决定，不是自由参数：
         *
         *   支撑脚在世界坐标里静止  ⇔  身体前进 = 脚在机体坐标里后退
         *   stride = v · T_stance = v · duty · T_cycle
         *   ⇒ omega = v · duty / stride
         *
         * 早先写成 omega = v / stride，脚后退得比身体前进快，
         * 于是**必然打滑**：v=0.2、duty=0.6、stride=0.08 时实测滑移 32 mm。
         * 那不是调参问题，是把「看起来在走」当成了「真的在走」。
         */
        if (q->speed > 0 && q->stride > 0) {
                motion_fixed_t vd, omega;

                if (fixed_mul_checked(q->speed, q->duty, &vd) != FIXED_OK)
                        return false;
                if (fixed_div_checked(vd, q->stride, &omega) != FIXED_OK)
                        return false;
                if (omega <= 0)
                        return false;
                if (fixed_mul_checked(omega, dt_s, &advance) != FIXED_OK)
                        return false;
                if (fixed_add_checked(q->phase, advance, &q->phase) != FIXED_OK)
                        return false;
                q->phase = frac_q(q->phase);
        }

        for (u8 i = 0; i < q->count; i++) {
                motion_fixed_t lp = frac_q(q->phase + q->legs[i].phase);
                motion_fixed_t half_stride, xb, yb;

                if (fixed_div_checked(q->stride, (motion_fixed_t)2 << 32,
                                      &half_stride) != FIXED_OK)
                        return false;

                if (lp < q->duty) {
                        /*
                         * 支撑相：机体坐标里以速度 -v 后退。
                         * 身体前进 + 脚后退 = 世界坐标静止。
                         */
                        motion_fixed_t s, back;

                        in_stance[i] = true;

                        if (fixed_div_checked(lp, q->duty, &s) != FIXED_OK)
                                return false;
                        if (fixed_mul_checked(q->stride, s, &back) != FIXED_OK)
                                return false;
                        if (fixed_sub_checked(half_stride, back, &xb) != FIXED_OK)
                                return false;

                        yb = -q->height;        /* 贴地：y 恒为 -height */
                } else {
                        /*
                         * 摆动相：从 -stride/2 前进到 +stride/2，
                         * 并抬起一个正弦拱形（起落速度为 0，避免落地冲击）。
                         */
                        motion_fixed_t s, span, adv, arc, s_arc, c_arc, lift_z;

                        in_stance[i] = false;

                        /* s = (lp − duty) / (1 − duty)，归一化到 0..1 */
                        if (fixed_sub_checked(lp, q->duty, &s) != FIXED_OK)
                                return false;
                        if (fixed_sub_checked((motion_fixed_t)1 << 32, q->duty,
                                              &span) != FIXED_OK)
                                return false;
                        if (span <= 0)
                                return false;
                        if (fixed_div_checked(s, span, &s) != FIXED_OK)
                                return false;

                        /* xb 从 −stride/2 线性走到 +stride/2 */
                        if (fixed_mul_checked(q->stride, s, &adv) != FIXED_OK)
                                return false;
                        if (fixed_sub_checked(adv, half_stride, &xb) != FIXED_OK)
                                return false;

                        /* 拱形：sin(π·s)，两端为 0，落地与抬脚都不带冲击 */
                        if (fixed_mul_checked(s, FIXED_PI, &arc) != FIXED_OK)
                                return false;
                        if (fixed_sin_cos(arc, &s_arc, &c_arc) != FIXED_OK)
                                return false;
                        if (fixed_mul_checked(q->lift, s_arc, &lift_z) != FIXED_OK)
                                return false;

                        {
                                motion_fixed_t ground = -q->height;

                                if (fixed_add_checked(ground, lift_z, &yb) != FIXED_OK)
                                        return false;
                        }
                }

                if (fixed_add_checked(q->legs[i].x0, xb, &targets[i][0]) != FIXED_OK)
                        return false;
                if (fixed_add_checked(q->legs[i].y0, yb, &targets[i][1]) != FIXED_OK)
                        return false;
        }

        q->steps++;
        return true;
}
