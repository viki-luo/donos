/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * control.h — 控制律：只读状态、只发原语
 *
 * 控制器（大脑的一部分）做的事只有两件：
 *   1. 通过 object_state() / object_resolve() 读**实测**；
 *   2. 通过 base_apply() 发一条运动原语。
 *
 * 它不能写物件状态、不能碰基座内部、不能保存物件的历史。
 * 换控制器就是换命令来源：拔掉旧的大脑插上新的，基座一行不改。
 *
 * 两个参考控制律：
 *   wheel_control  速度环：目标线速度 → 误差 → 线速度命令
 *   joint_control  位置环：目标角 → 误差 → 角速度命令
 *
 * 两者都只做 P 控制并限幅。这不是「工业级控制」，而是**证明控制回路
 * 真的闭合**：读的是实测、命令经过唯一原语、误差能把输出推向目标。
 */
#ifndef DONOS_CONTROL_H
#define DONOS_CONTROL_H

#include "base.h"
#include "object.h"

/* 控制器的统一形状：目标、增益、限幅。类型不写进结构 —— 结构只描述回路。 */
typedef struct {
        wire_t         wire;          /* 被控的那条接线 */
        u64            period_ns;     /* 控制器自己的节拍（与物件周期可不同） */
        motion_fixed_t target;        /* 目标：线速度 m/s 或角度 rad */
        motion_fixed_t gain;          /* P 增益（无量纲） */
        motion_fixed_t limit;         /* 输出限幅（与命令同量纲） */

        u64            last_run_ns;   /* 上次计算时刻，用于判断是否到期 */
        u32            commands_sent; /* 统计：发过多少条命令 */
        u32            refusals;      /* 统计：被基座拒绝多少次 */
} control_loop_t;

/*
 * 速度环：command = 目标 + K·(目标 − 实测)。
 *
 * 前馈项不是修饰：只做比例时，一阶对象上的静止点是 target/(1+K)，
 * 目标 1、K=0.5 会停在 0.333。加上前馈后静止点解出 actual = target，
 * 稳态误差为零 —— 这是解析结论，测试里按 0.333 与 1.0 两个值核对。
 */
bool wheel_control_init(control_loop_t *c, wire_t w, u64 period_ns,
                        motion_fixed_t target_speed, motion_fixed_t gain,
                        motion_fixed_t limit);

/*
 * 推进一步。返回 true 表示这一步真的发出了命令。
 *
 * now_ns 由调用者给出 —— 控制器不读时钟，否则它就成了另一个时间源。
 * 未到期、读不到状态、误差为零时返回 false 且不发命令：
 * 「没有命令」和「命令为零」是两件不同的事。
 */
bool wheel_control_step(control_loop_t *c, u64 now_ns);

/* 位置环：误差 = 目标角 − 实测角（取自姿态四元数）；输出角速度命令 */
bool joint_control_init(control_loop_t *c, wire_t w, u64 period_ns,
                        motion_fixed_t target_angle, motion_fixed_t gain,
                        motion_fixed_t limit);

bool joint_control_step(control_loop_t *c, u64 now_ns);

/*
 * 从姿态四元数里取出绕 z 的转角：θ = 2·atan2(qz, qw)。
 *
 * atan2 用 CORDIC 向量模式实现（见 control.c 的 cordic_atan2）：
 * 只有移位、加法与一张 16 项的 atan(2^-i) 表，没有除法调用、没有浮点。
 * 角度残差约 3e-5 rad。
 *
 * 历史：早先这里用的是小角度近似 θ ≈ 2·qz，在 |θ| > 0.6 rad 时误差超过 2%，
 * 而且代码里就写着「这是已知近似」。近似已被替换，测试里用 1.5 rad、
 * 近 π 与负角度三条断言证明这一点 —— 那些正是近似做不到的地方。
 */
bool joint_angle_from_orientation(const motion_fixed_t q[4],
                                  motion_fixed_t *angle_rad);

#endif /* DONOS_CONTROL_H */
