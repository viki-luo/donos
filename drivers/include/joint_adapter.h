/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * joint_adapter.h — 关节：角速度 ↔ 关节电机
 *
 * 一条接线 = 一个关节，这是刻意的最小单元：
 *   机械臂有 6 个关节就是 6 条接线，机械腿也一样。
 *   不把它做成「一条 dof=12 的接线」，因为基座的 dof 上限是 6，
 *   而且串联关节的参照系本来就是逐级传递的 —— 第 2 关节相对第 1 关节，
 *   第 1 关节相对世界。用接线表达这条链，比用一条超宽命令表达更诚实。
 *
 * 换算全在这里：
 *   角速度 ω (rad/s) → 电机 counts/s
 *   编码器计数差       → 实测角度（相对本关节的零位）
 *   行程限制           → 由物件自己的能力申报表达，适配器只做换算
 */
#ifndef DONOS_JOINT_ADAPTER_H
#define DONOS_JOINT_ADAPTER_H

#include "motion_device.h"

/*
 * counts_per_rad  每弧度对应的编码器计数（Q32.32）
 * zero_counts     关节零位对应的计数（Q32.32）
 * invert          安装方向相反时置 1
 */
typedef struct {
        motion_fixed_t counts_per_rad;
        motion_fixed_t zero_counts;
        bool           invert;
} joint_params_t;

typedef struct {
        joint_params_t params;

        bool (*motor_set_counts)(void *ctx, motion_fixed_t counts_per_s);
        bool (*encoder_read)(void *ctx, motion_fixed_t *counts);
        void *ctx;

        motion_fixed_t last_counts;
        bool           primed;
        u64            period_ns;
} joint_adapter_t;

bool joint_attach(wire_t w, joint_adapter_t *ja);
void joint_detach(wire_t w, joint_adapter_t *ja);

/* 纯换算，供宿主测试按手算值核对 */
bool joint_speed_to_counts(const joint_params_t *p, motion_fixed_t omega_rad_s,
                           motion_fixed_t *counts_per_s);
bool joint_counts_to_angle(const joint_params_t *p, motion_fixed_t counts,
                           motion_fixed_t *angle_rad);

#endif /* DONOS_JOINT_ADAPTER_H */
