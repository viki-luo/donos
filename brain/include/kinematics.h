/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kinematics.h — 平面二连杆正运动学（大脑侧，不进基座）
 *
 * 为什么它在大脑里而不在基座里：
 *   基座只认「接线是否有效、运动命令是否新鲜」。腿长、关节夹角、
 *   足端坐标是这条腿自己的几何学 —— 属于上层大脑对实测状态的解释。
 *   把腿长放进基座，等于让基座开始理解「这条接线是一条腿」，
 *   而那正是 DONOS-MO-1 拒绝的设备类型字段。
 *
 * 输入是**实测**关节角（从姿态四元数取出），不是命令角：
 * 用命令角算出的足端位置描述的是「我希望它在哪」，不是「它在哪」。
 */
#ifndef DONOS_KINEMATICS_H
#define DONOS_KINEMATICS_H

#include "fixed.h"
#include "object.h"

/* 二连杆几何：l1 从髋到膝，l2 从膝到足，单位米（Q32.32） */
typedef struct {
        motion_fixed_t l1;
        motion_fixed_t l2;
} leg_geometry_t;

/*
 * 足端位置（相对髋、世界坐标）。
 *
 *   x = l1·cos(θ1) + l2·cos(θ1 + θ2)
 *   y = l1·sin(θ1) + l2·sin(θ1 + θ2)
 *
 * 两个角度都是绕 z 的转角，单位弧度。
 * 返回 false 表示参数非法或数值溢出。
 */
bool leg_tip(const leg_geometry_t *g, motion_fixed_t theta1,
             motion_fixed_t theta2, motion_fixed_t *x, motion_fixed_t *y);

/*
 * 可达性检查：足端是否落在圆环 [|l1−l2|, l1+l2] 内。
 *
 * 这是**几何上必然成立的不变量**，不依赖任何控制逻辑：
 * 若一次步态让足端跑到这个范围之外，那不是「工作空间设定得不对」，
 * 而是角度值本身被算错了。
 */
bool leg_reachable(const leg_geometry_t *g, motion_fixed_t x, motion_fixed_t y);

/* 到髋的距离平方（Q32.32），用于上面那条不变量，避免开方 */
bool leg_radius_squared(motion_fixed_t x, motion_fixed_t y,
                        motion_fixed_t *out);

#endif /* DONOS_KINEMATICS_H */
