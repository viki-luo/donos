/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gait.h — 关节轨迹生成（步态）
 *
 * 步态只做一件事：随时间产生**每个关节的目标角**。
 * 它不下命令、不碰基座、不读传感器 —— 下命令是控制器的事，
 * 传感器数据属于物件。这样「相位从哪来」可以整体替换，
 * 而控制回路与基座都不用改。
 */
#ifndef DONOS_GAIT_H
#define DONOS_GAIT_H

#include "control.h"

#define GAIT_JOINTS_MAX 4

typedef struct {
        wire_t         wire;
        motion_fixed_t centre;      /* 中位角（rad） */
        motion_fixed_t amplitude;   /* 摆幅（rad） */
        motion_fixed_t phase;       /* 相位偏移（rad） */
} gait_joint_t;

typedef struct {
        gait_joint_t   joints[GAIT_JOINTS_MAX];
        u8             count;
        u64            period_ns;   /* 步态自身的更新周期 */
        motion_fixed_t omega;       /* 相位角速度（rad/s） */
        motion_fixed_t phase;       /* 当前相位（rad） */
        u64            last_ns;
        u32            steps;
} gait_t;

/*
 * 起点。每个关节给三个数：中位角、摆幅、相位偏移。
 * 频率由 omega 给出（rad/s），与关节的控制周期无关。
 */
bool gait_init(gait_t *g, u64 period_ns, motion_fixed_t omega);

/* 追加一个关节。超过上限、参数非法返回 false。 */
bool gait_add_joint(gait_t *g, wire_t w, motion_fixed_t centre,
                    motion_fixed_t amplitude, motion_fixed_t phase);

/*
 * 推进相位并算出当前目标角。
 *
 * now_ns 由调用者给出；返回 true 表示这一步确实更新了相位。
 * 输出 targets[i] 与 joints[i] 一一对应。
 */
bool gait_step(gait_t *g, u64 now_ns, motion_fixed_t *targets,
               u8 targets_capacity);

#endif /* DONOS_GAIT_H */
