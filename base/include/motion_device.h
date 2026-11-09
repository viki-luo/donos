/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * motion_device.h — 物件背后的真实设备
 *
 * 为什么需要这一层：
 *   参考实现里物件的控制环就是它自己，于是「刚被命令的速度」被直接
 *   抄成「实测速度」。那在模拟里成立，在真实电机上不成立 —— 电机可能
 *   堵转、丢步、被外力拖住、编码器接反，实测与命令本来就不是一回事。
 *
 * 所以真实设备通过这组回调接进物件侧：
 *   set_velocity  把速度命令交给电机（单位 m/s、rad/s）
 *   sample        从编码器/传感器读回**实测**位姿与速度
 *
 * 两条纪律：
 *   1. 单位换算（轮径、减速比、编码器线数、关节零位、安装方向）只在
 *      设备实现里做，物件侧与基座只见 m/s 与 rad/s。
 *   2. 实测只来自 sample()。命令永远不写进实测字段 —— 否则监视器
 *      看到的「偏差」永远是零，偏差检测就成了摆设。
 */
#ifndef DONOS_MOTION_DEVICE_H
#define DONOS_MOTION_DEVICE_H

#include "object.h"

typedef struct {
        /*
         * 把速度命令交给设备。linear 与 angular 都是本物件相对 frame 的
         * 速度（m/s、rad/s），长度为 MOTION_DOF_MAX / 2。
         * 返回 false 表示设备拒绝或不在线；调用者仍会保留命令，
         * 但不会因此伪造一份实测。
         */
        bool (*set_velocity)(void *ctx,
                             const motion_fixed_t linear[3],
                             const motion_fixed_t angular[3]);

        /*
         * 读回实测位姿与速度，**覆盖** out 的全部字段。
         * 返回 false 表示读不到 —— 此时调用者必须保持上一次的实测值，
         * 而不是把它当成零（零速度会被误读成「已经停住」）。
         */
        bool (*sample)(void *ctx, object_state_t *out);
} motion_device_ops_t;

#endif /* DONOS_MOTION_DEVICE_H */
