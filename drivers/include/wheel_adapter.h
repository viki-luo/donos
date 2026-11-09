/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * wheel_adapter.h — 车轮：线速度 ↔ 电机转速 ↔ 编码器计数
 *
 * 为什么换算必须在这里：
 *   基座与物件只见 m/s。轮半径、减速比、编码器每转计数、安装方向
 *   都属于「这台具体设备怎么把 m/s 变成转」。把它们放进物件侧或基座，
 *   换一个轮子就要改模型 —— 那正是 DONos 要避免的事。
 *
 * 换算链（线速度 → 电机）：
 *   v (m/s) → ω_wheel = v / r (rad/s)
 *           → ω_motor = ω_wheel × gear
 *           → counts/s = ω_motor / 2π × counts_per_rev
 *
 * 反向（编码器 → 实测）：
 *   Δcounts → Δangle = Δcounts / counts_per_rev × 2π
 *           → Δposition = Δangle × r / gear
 *
 * 车轮是差速驱动的最小单元：每个轮子一条接线，方向由 signed 线速度表达。
 * 倒车就是负的 motion[0]，没有「倒车指令」这回事。
 */
#ifndef DONOS_WHEEL_ADAPTER_H
#define DONOS_WHEEL_ADAPTER_H

#include "motion_device.h"

/*
 * 一台车轮设备的静态参数。
 *
 * radius         轮半径（Q32.32，m）
 * gear_ratio     电机 : 轮 的减速比（Q32.32，≥1 表示减速）
 * counts_per_rev 轮轴每转一圈的编码器计数（Q32.32）
 * invert         编码器/电机方向相反时置 1（例如左右轮镜像安装）
 */
typedef struct {
        motion_fixed_t radius;
        motion_fixed_t gear_ratio;
        motion_fixed_t counts_per_rev;
        bool           invert;
} wheel_params_t;

typedef struct {
        wheel_params_t params;

        /* 由设备实现提供的底层 I/O，adapter 不直接碰硬件 */

        /* 把电机目标转速（counts/s）下发；返回 false 表示设备拒绝 */
        bool (*motor_set_counts)(void *ctx, motion_fixed_t counts_per_s);

        /* 读编码器累计计数；返回 false 表示读不到 */
        bool (*encoder_read)(void *ctx, motion_fixed_t *counts);

        void *ctx;

        /* adapter 自己的状态 */
        motion_fixed_t last_counts;
        bool           primed;
        u64            period_ns;      /* 物件申报的控制周期，挂接时读入 */
} wheel_adapter_t;

/*
 * 把适配器接到一条接线上。接线无效、参数非法或回调缺失返回 false。
 * 参数校验：radius > 0、gear_ratio > 0、counts_per_rev > 0。
 */
bool wheel_attach(wire_t w, wheel_adapter_t *wa);
void wheel_detach(wire_t w, wheel_adapter_t *wa);

/*
 * 纯换算函数：暴露出来是为了能在宿主侧按手算值测试，
 * 不必为了验证一条公式就搭一整套假硬件。
 */
bool wheel_speed_to_counts(const wheel_params_t *p, motion_fixed_t v_mps,
                           motion_fixed_t *counts_per_s);
bool wheel_counts_to_speed(const wheel_params_t *p, motion_fixed_t counts_per_s,
                           motion_fixed_t *v_mps);

#endif /* DONOS_WHEEL_ADAPTER_H */
