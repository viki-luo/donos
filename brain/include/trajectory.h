/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * trajectory.h — 连续足端轨迹
 *
 * 与步态（gait）的分工：
 *   gait       产生**关节角**目标，用在摆动相这种「关节怎么摆」的场合
 *   trajectory 产生**足端位置**目标，用在「脚要沿这条线走」的场合
 * 后者更接近任务本身：调用者说的是脚的运动，不是关节的运动，
 * 关节角由逆解从足端位置反求。
 *
 * 这里只做平面圆。选圆而不是直线，是因为圆处处改变方向：
 * 直线轨迹会让「跟踪滞后」恰好沿运动方向、容易被误读成常值偏移，
 * 圆的滞后方向一直变，误差才是真的被跟踪出来的。
 */
#ifndef DONOS_TRAJECTORY_H
#define DONOS_TRAJECTORY_H

#include "fixed.h"

typedef struct {
        motion_fixed_t cx;
        motion_fixed_t cy;
        motion_fixed_t radius;
        motion_fixed_t omega;       /* 角速度（rad/s），符号决定方向 */
        motion_fixed_t phase;       /* 当前相位 */
        u64            last_ns;
        u32            steps;
} circular_path_t;

bool path_init_circle(circular_path_t *p, motion_fixed_t cx, motion_fixed_t cy,
                      motion_fixed_t radius, motion_fixed_t omega);

/*
 * 推进相位并给出当前足端目标。
 *
 * 第一次调用只记时刻、返回 false —— 轨迹从哪里开始是调用者的事。
 * now_ns 由调用者给出：轨迹不读时钟，否则它就成了第二个时间源。
 */
bool path_step_circle(circular_path_t *p, u64 now_ns,
                      motion_fixed_t *x, motion_fixed_t *y);

#endif /* DONOS_TRAJECTORY_H */
