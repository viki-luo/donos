/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * wheel_adapter.c — 车轮单位换算与设备回调
 *
 * 全部换算走 checked 定点层，不出现浮点，也不让一次除法溢出悄悄变成
 * 一个巨大的速度命令。2π 与 1e9 之类常量都在编译期定好。
 */
#include "fixed.h"
#include "wheel_adapter.h"

/*
 * 2π ≈ 6.283185307，Q32.32。
 * 用整数常量而非浮点字面量：freestanding 构建没有软浮点运行时。
 */
#define TWO_PI_Q  ((motion_fixed_t)26986015494LL)

static bool wheel_params_valid(const wheel_params_t *p)
{
        return p != 0 && p->radius > 0 && p->gear_ratio > 0 &&
               p->counts_per_rev > 0;
}

bool wheel_speed_to_counts(const wheel_params_t *p, motion_fixed_t v_mps,
                           motion_fixed_t *counts_per_s)
{
        motion_fixed_t v, omega_wheel, omega_motor, scaled;

        if (!wheel_params_valid(p) || counts_per_s == 0)
                return false;

        v = p->invert ? -v_mps : v_mps;

        /* ω_wheel = v / r —— checked 层没有除法，乘倒数。 */
        if (fixed_div_checked(v, p->radius, &omega_wheel) != FIXED_OK)
                return false;

        /* ω_motor = ω_wheel × gear */
        if (fixed_mul_checked(omega_wheel, p->gear_ratio, &omega_motor) != FIXED_OK)
                return false;

        /* counts/s = ω_motor / 2π × counts_per_rev */
        if (fixed_div_checked(omega_motor, TWO_PI_Q, &scaled) != FIXED_OK)
                return false;
        if (fixed_mul_checked(scaled, p->counts_per_rev, counts_per_s) != FIXED_OK)
                return false;

        return true;
}

bool wheel_counts_to_speed(const wheel_params_t *p, motion_fixed_t counts_per_s,
                           motion_fixed_t *v_mps)
{
        motion_fixed_t revs, omega_motor, omega_wheel, v;

        if (!wheel_params_valid(p) || v_mps == 0)
                return false;

        /* revs/s = counts/s ÷ counts_per_rev */
        if (fixed_div_checked(counts_per_s, p->counts_per_rev, &revs) != FIXED_OK)
                return false;

        /* ω_motor = revs/s × 2π */
        if (fixed_mul_checked(revs, TWO_PI_Q, &omega_motor) != FIXED_OK)
                return false;

        /* ω_wheel = ω_motor ÷ gear */
        if (fixed_div_checked(omega_motor, p->gear_ratio, &omega_wheel) != FIXED_OK)
                return false;

        /* v = ω_wheel × r */
        if (fixed_mul_checked(omega_wheel, p->radius, &v) != FIXED_OK)
                return false;

        *v_mps = p->invert ? -v : v;
        return true;
}

static bool wheel_set_velocity(void *ctx, const motion_fixed_t linear[3],
                               const motion_fixed_t angular[3])
{
        wheel_adapter_t *wa = (wheel_adapter_t *)ctx;
        motion_fixed_t counts_per_s;

        (void)angular;          /* 单轮不带转向轴；转向由差速表达 */

        if (wa == 0 || wa->motor_set_counts == 0)
                return false;
        if (!wheel_speed_to_counts(&wa->params, linear[0], &counts_per_s))
                return false;

        return wa->motor_set_counts(wa->ctx, counts_per_s);
}

static bool wheel_sample(void *ctx, object_state_t *out)
{
        wheel_adapter_t *wa = (wheel_adapter_t *)ctx;
        motion_fixed_t counts, delta, speed;
        motion_fixed_t counts_per_s;

        if (wa == 0 || wa->encoder_read == 0 || out == 0)
                return false;
        if (!wa->encoder_read(wa->ctx, &counts))
                return false;

        for (u8 i = 0; i < 3; i++) {
                out->linear[i] = 0;
                out->angular[i] = 0;
                out->position[i] = 0;
        }
        for (u8 i = 0; i < 4; i++)
                out->orientation[i] = 0;

        /*
         * 第一次采样只建立基准：此时没有「上一次计数」，任何位移都是
         * 凭空造出来的。这与执行层「首次只排期」是同一条纪律。
         */
        if (!wa->primed) {
                wa->last_counts = counts;
                wa->primed = true;
                return true;
        }

        if (fixed_sub_checked(counts, wa->last_counts, &delta) != FIXED_OK)
                return false;
        wa->last_counts = counts;

        /*
         * 位置：把累计计数按轮周长折算。
         * counts_per_rev 是「每转计数」，所以一圈的位移是 2πr / gear。
         */
        {
                motion_fixed_t per_rev, travelled;

                if (fixed_div_checked((motion_fixed_t)1 << 32,
                                      wa->params.counts_per_rev, &per_rev) != FIXED_OK)
                        return false;
                if (fixed_mul_checked(per_rev, TWO_PI_Q, &travelled) != FIXED_OK)
                        return false;
                if (fixed_div_checked(travelled, wa->params.gear_ratio,
                                      &travelled) != FIXED_OK)
                        return false;
                if (fixed_mul_checked(travelled, wa->params.radius,
                                      &travelled) != FIXED_OK)
                        return false;
                if (fixed_mul_checked(delta, travelled, &travelled) != FIXED_OK)
                        return false;

                out->position[0] = wa->params.invert ? -travelled : travelled;
        }

        /*
         * 速度：delta 是「本次周期内的计数差」，不是 counts/s。
         * 除以本物件申报的周期才是速度 —— 把 delta 直接当速度，
         * 会让同一条命令在不同周期下报出不同的「实测」，纯属量纲错误。
         */
        if (wa->period_ns != 0) {
                motion_fixed_t period_q;

                /* period_q = period_ns / 1e9，Q32.32 */
                period_q = (motion_fixed_t)((u128)wa->period_ns << 32) /
                           (motion_fixed_t)1000000000;

                if (period_q != 0 &&
                    fixed_div_checked(delta, period_q, &counts_per_s) == FIXED_OK &&
                    wheel_counts_to_speed(&wa->params, counts_per_s, &speed))
                        out->linear[0] = speed;
        }

        return true;
}

static const motion_device_ops_t wheel_ops = {
        .set_velocity = wheel_set_velocity,
        .sample = wheel_sample,
};

bool wheel_attach(wire_t w, wheel_adapter_t *wa)
{
        u32 unit = 0;
        u64 period = 0;

        if (wa == 0 || !wheel_params_valid(&wa->params))
                return false;

        /*
         * 周期来自物件自己的申报，不是适配器猜的：单位换算要知道
         * 「这一步是多长时间」，而这个时间只有物件自述得准。
         */
        if (object_declared(w, &unit, &period) && period != 0)
                wa->period_ns = period;
        else
                wa->period_ns = 0;

        wa->primed = false;
        wa->last_counts = 0;

        return object_attach_device(w, &wheel_ops, wa);
}

void wheel_detach(wire_t w, wheel_adapter_t *wa)
{
        (void)wa;
        object_detach_device(w);
}
