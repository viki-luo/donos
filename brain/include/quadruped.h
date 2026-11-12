/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * quadruped.h — 四足步态：支撑相与摆动相
 *
 * 一条腿就是两条接线（髋、膝），四条腿就是八条接线。这里没有任何
 * 「机器人」对象，也没有一个地方写着「这是前左腿」—— 那只是索引，
 * 基座看不见索引，它只看见八条线。
 *
 * 关键物理约定（决定了「不滑行」能否成立）：
 *   **支撑相里，脚在世界坐标中静止**。
 *   若支撑脚跟着身体一起动，机器人在原地打滑，看起来像在走，
 *   实际上一点位移都没换来 —— 这是四足步态最容易假装成功的地方。
 *
 * 实现上用一条恒等式保证它：
 *   身体以速度 v 前进，支撑脚在**机体坐标**里以 -v 后退，
 *   两者相加恰好抵消，世界坐标里脚不动。
 *
 * 这条恒等式规定了三个参数**不能独立给**：
 *   stride = v · T_stance = v · duty · T_cycle
 *   ⇒ 相位频率 omega = v · duty / stride
 * 把 omega 当成自由参数（例如取 v/stride）会让脚后退得比身体快，
 * 结果就是原地打滑 —— 八条接线都在动，位移却一点没换来。
 *
 * 平面约定：x 向前，y 向上（地面在 y = -body_height）。
 */
#ifndef DONOS_QUADRUPED_H
#define DONOS_QUADRUPED_H

#include "control.h"
#include "kinematics.h"

#define QUAD_LEGS_MAX 4

typedef struct {
        wire_t         hip;
        wire_t         knee;
        leg_geometry_t geom;
        motion_fixed_t phase;       /* 该腿的相位偏移，0..1 */
        motion_fixed_t x0;          /* 髋在机体坐标里的前后位置 */
        motion_fixed_t y0;          /* 髋在机体坐标里的高度偏移 */
        bool           installed;
} quad_leg_t;

typedef struct {
        quad_leg_t     legs[QUAD_LEGS_MAX];
        u8             count;
        u64            period_ns;
        motion_fixed_t speed;       /* 身体前进速度 m/s */
        motion_fixed_t duty;        /* 支撑相占比，0..1（0.5 = 半支撑） */
        motion_fixed_t stride;      /* 步幅 m */
        motion_fixed_t lift;        /* 抬脚高度 m */
        motion_fixed_t height;      /* 机身高度 m（脚在地面 y = -height） */
        motion_fixed_t phase;       /* 全局相位，0..1 */
        motion_fixed_t body_x;      /* 身体前进位移 */
        u64            last_ns;
        u32            steps;
} quadruped_t;

bool quad_init(quadruped_t *q, u64 period_ns, motion_fixed_t speed,
               motion_fixed_t duty, motion_fixed_t stride,
               motion_fixed_t lift, motion_fixed_t height);

/* 追加一条腿。phase 是该腿的相位偏移（对角小跑：0, 0.5, 0.5, 0）。 */
bool quad_add_leg(quadruped_t *q, wire_t hip, wire_t knee,
                  const leg_geometry_t *geom, motion_fixed_t phase,
                  motion_fixed_t x0, motion_fixed_t y0);

/*
 * 推进一步：推进相位与身体位移，输出每条腿的**足端目标**（机体坐标）。
 *
 * targets[i][0] = 前后，targets[i][1] = 高度（负值在地面以下的方向）。
 * 同时 outputs[i] = true 表示该腿此刻处于支撑相。
 */
bool quad_step(quadruped_t *q, u64 now_ns,
               motion_fixed_t targets[QUAD_LEGS_MAX][2],
               bool in_stance[QUAD_LEGS_MAX]);

/* 该腿此刻是否在支撑相（给定全局相位）。 */
bool quad_leg_in_stance(const quadruped_t *q, u8 leg);

#endif /* DONOS_QUADRUPED_H */
