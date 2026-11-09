/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * joint_adapter.c — 关节单位换算与设备回调
 *
 * 关节只关心一个量：绕自身轴的角速度（rad/s）。
 * 编码器给的是计数，电机收的是 counts/s，中间隔着 counts_per_rad。
 */
#include "fixed.h"
#include "joint_adapter.h"

static bool joint_params_valid(const joint_params_t *p)
{
        return p != 0 && p->counts_per_rad > 0;
}

bool joint_speed_to_counts(const joint_params_t *p, motion_fixed_t omega_rad_s,
                           motion_fixed_t *counts_per_s)
{
        if (!joint_params_valid(p) || counts_per_s == 0)
                return false;

        motion_fixed_t omega = p->invert ? -omega_rad_s : omega_rad_s;

        return fixed_mul_checked(omega, p->counts_per_rad, counts_per_s) == FIXED_OK;
}

bool joint_counts_to_angle(const joint_params_t *p, motion_fixed_t counts,
                           motion_fixed_t *angle_rad)
{
        motion_fixed_t relative, angle;

        if (!joint_params_valid(p) || angle_rad == 0)
                return false;

        /* 相对零位的计数 */
        if (fixed_sub_checked(counts, p->zero_counts, &relative) != FIXED_OK)
                return false;

        if (fixed_div_checked(relative, p->counts_per_rad, &angle) != FIXED_OK)
                return false;

        *angle_rad = p->invert ? -angle : angle;
        return true;
}

static bool joint_set_velocity(void *ctx, const motion_fixed_t linear[3],
                               const motion_fixed_t angular[3])
{
        joint_adapter_t *ja = (joint_adapter_t *)ctx;
        motion_fixed_t counts_per_s;

        (void)linear;           /* 关节不产生平移 */

        if (ja == 0 || ja->motor_set_counts == 0)
                return false;

        /* 关节只取绕自身 z 轴的角速度分量 */
        if (!joint_speed_to_counts(&ja->params, angular[2], &counts_per_s))
                return false;

        return ja->motor_set_counts(ja->ctx, counts_per_s);
}

static bool joint_sample(void *ctx, object_state_t *out)
{
        joint_adapter_t *ja = (joint_adapter_t *)ctx;
        motion_fixed_t counts, delta, angle, counts_per_s;
        motion_fixed_t omega;

        if (ja == 0 || ja->encoder_read == 0 || out == 0)
                return false;
        if (!ja->encoder_read(ja->ctx, &counts))
                return false;

        for (u8 i = 0; i < 3; i++) {
                out->linear[i] = 0;
                out->angular[i] = 0;
                out->position[i] = 0;
        }
        for (u8 i = 0; i < 4; i++)
                out->orientation[i] = 0;

        /*
         * 绝对角度写成**姿态四元数**，不写 position[]：
         * position 按 DONOS-MO-1 的 Q-2 是**米**，把弧度塞进去是单位违规。
         * 姿态是 object_state 规定的旋转表示，所以关节角属于 orientation。
         *
         * q = (cos(θ/2), 0, 0, sin(θ/2)) —— 绕 z 轴转 θ。
         * 取半角需要一次除法，三角函数用定点实现。
         */
        if (joint_counts_to_angle(&ja->params, counts, &angle)) {
                motion_fixed_t half, sh, ch;

                if (fixed_div_checked(angle, (motion_fixed_t)2 << 32, &half) == FIXED_OK &&
                    fixed_sin_cos(half, &sh, &ch) == FIXED_OK) {
                        out->orientation[0] = ch;
                        out->orientation[1] = 0;
                        out->orientation[2] = 0;
                        out->orientation[3] = sh;
                }
        }

        /*
         * 第一次采样只建立基准：没有「上一次计数」就没有速度可言。
         * 与执行层「首次只排期」是同一条纪律。
         */
        if (!ja->primed) {
                ja->last_counts = counts;
                ja->primed = true;
                return true;
        }

        if (fixed_sub_checked(counts, ja->last_counts, &delta) != FIXED_OK)
                return false;
        ja->last_counts = counts;

        /* 角速度 = 计数差 ÷ 周期 ÷ counts_per_rad */
        if (ja->period_ns != 0) {
                motion_fixed_t period_q;

                period_q = (motion_fixed_t)((u128)ja->period_ns << 32) /
                           (motion_fixed_t)1000000000;

                if (period_q != 0 &&
                    fixed_div_checked(delta, period_q, &counts_per_s) == FIXED_OK &&
                    fixed_div_checked(counts_per_s, ja->params.counts_per_rad,
                                      &omega) == FIXED_OK)
                        out->angular[2] = ja->params.invert ? -omega : omega;
        }

        return true;
}

static const motion_device_ops_t joint_ops = {
        .set_velocity = joint_set_velocity,
        .sample = joint_sample,
};

bool joint_attach(wire_t w, joint_adapter_t *ja)
{
        u32 unit = 0;
        u64 period = 0;

        if (ja == 0 || !joint_params_valid(&ja->params))
                return false;

        if (object_declared(w, &unit, &period) && period != 0)
                ja->period_ns = period;
        else
                ja->period_ns = 0;

        ja->primed = false;
        ja->last_counts = 0;

        return object_attach_device(w, &joint_ops, ja);
}

void joint_detach(wire_t w, joint_adapter_t *ja)
{
        (void)ja;
        object_detach_device(w);
}
