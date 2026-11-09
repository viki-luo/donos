/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * control.c — 速度环与位置环
 *
 * 两条纪律贯穿全文：
 *   1. 只读实测。命令与实测是两件事；拿命令做反馈等于自己骗自己。
 *   2. 只发原语。控制器不写物件状态、不碰基座，任何「直接设定位置」
 *      的念头都在这里被排除 —— 那会绕开唯一控制面。
 */
#include "control.h"
#include "fixed.h"

/* 输出限幅：把任意实数夹到 [-limit, +limit] */
static motion_fixed_t clamp(motion_fixed_t v, motion_fixed_t limit)
{
        if (limit <= 0)
                return v;
        if (v > limit)
                return limit;
        if (v < -limit)
                return -limit;
        return v;
}

static bool due(control_loop_t *c, u64 now_ns)
{
        if (c->last_run_ns == 0) {
                /*
                 * 第一次调用只记时刻、不计算：控制器还没见过实测，
                 * 此刻的任何误差都是凭空造的。与执行层「首次只排期」同理。
                 */
                c->last_run_ns = now_ns;
                return false;
        }
        if (c->period_ns == 0)
                return false;
        if (now_ns < c->last_run_ns || now_ns - c->last_run_ns < c->period_ns)
                return false;

        c->last_run_ns = now_ns;
        return true;
}

bool wheel_control_init(control_loop_t *c, wire_t w, u64 period_ns,
                        motion_fixed_t target_speed, motion_fixed_t gain,
                        motion_fixed_t limit)
{
        if (c == 0)
                return false;
        if (!base_wire_valid(w))
                return false;
        if (period_ns == 0 || period_ns > 1000000000ULL)
                return false;
        if (gain <= 0 || limit <= 0)
                return false;

        c->wire = w;
        c->frame = FRAME_WORLD;
        c->period_ns = period_ns;
        c->target = target_speed;
        c->gain = gain;
        c->limit = limit;
        c->last_run_ns = 0;
        c->commands_sent = 0;
        c->refusals = 0;
        return true;
}

bool wheel_control_step(control_loop_t *c, u64 now_ns)
{
        object_state_t st;
        motion_fixed_t error, command;
        motion_event_t ev;

        if (c == 0 || !due(c, now_ns))
                return false;

        /* 读实测：读不到就不发命令，而不是假设它停着 */
        if (!object_state(c->wire, &st))
                return false;

        if (fixed_sub_checked(c->target, st.linear[0], &error) != FIXED_OK)
                return false;

        /*
         * 前馈 + 比例：command = target + K·(target − measured)。
         *
         * 纯比例环在一阶对象上会停在 target/(1+K)，不会到 target：
         * 目标 1、K=0.5 时稳态是 0.333，这是控制理论的必然结果，
         * 不是调参问题。把目标本身当前馈加上去，静止点解出
         * actual = target，稳态误差为零。
         */
        if (fixed_mul_checked(error, c->gain, &command) != FIXED_OK)
                return false;
        if (fixed_add_checked(c->target, command, &command) != FIXED_OK)
                return false;

        command = clamp(command, c->limit);

        /*
         * 误差为零时仍要发一条「零速度」命令：停住是明确意图，
         * 不能靠「不发命令」来表达 —— 那会让物件按窗口自然过期，
         * 而调用者无从知道是「要求停」还是「控制器失联」。
         */
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = 0;
        ev.wire = c->wire;
        ev.frame = c->frame;
        ev.issued_at_ns = now_ns;
        ev.duration_ns = c->period_ns * 2u;     /* 两拍窗口：允许丢一拍 */
        ev.dof = 1;
        ev.motion[0] = command;

        c->commands_sent++;
        if (base_apply(now_ns, &ev) != MOTION_OK)
                c->refusals++;

        return true;
}

bool joint_control_init(control_loop_t *c, wire_t w, u64 period_ns,
                        motion_fixed_t target_angle, motion_fixed_t gain,
                        motion_fixed_t limit)
{
        if (c == 0)
                return false;
        if (!base_wire_valid(w))
                return false;
        if (period_ns == 0 || period_ns > 1000000000ULL)
                return false;
        if (gain <= 0 || limit <= 0)
                return false;

        c->wire = w;
        c->frame = FRAME_WORLD;
        c->period_ns = period_ns;
        c->target = target_angle;
        c->gain = gain;
        c->limit = limit;
        c->last_run_ns = 0;
        c->commands_sent = 0;
        c->refusals = 0;
        return true;
}

/*
 * CORDIC 向量模式的 atan 表：atan(2^-i)，Q32.32。
 *
 * 表由公式生成，不是手抄；16 项在 Q32.32 下把角度误差压到约 3e-5 rad，
 * 对关节控制足够，且全程只有移位与加减 —— 没有除法调用，没有浮点。
 */
static const motion_fixed_t cordic_atan[16] = {
        3373259426LL,  /* atan(2^-0 ) */
        1991351318LL,  /* atan(2^-1 ) */
        1052175346LL,  /* atan(2^-2 ) */
        534100635LL,   /* atan(2^-3 ) */
        268086748LL,   /* atan(2^-4 ) */
        134174063LL,   /* atan(2^-5 ) */
        67103403LL,    /* atan(2^-6 ) */
        33553749LL,    /* atan(2^-7 ) */
        16777131LL,    /* atan(2^-8 ) */
        8388597LL,     /* atan(2^-9 ) */
        4194303LL,     /* atan(2^-10) */
        2097152LL,     /* atan(2^-11) */
        1048576LL,     /* atan(2^-12) */
        524288LL,      /* atan(2^-13) */
        262144LL,      /* atan(2^-14) */
        131072LL,      /* atan(2^-15) */
};

/* 绝对值，避免 -INT64_MIN 的边界 */
static motion_fixed_t abs_q(motion_fixed_t v)
{
        return v < 0 ? -(v + 1) + 1 : v;
}

/*
 * atan2(y, x)，Q32.32 输入输出，CORDIC 向量模式。
 *
 * 把向量 (x, y) 逐步旋转到 x 轴正半轴，累加的旋转角就是 atan2。
 * x < 0 时先取反并补 π；x == 0 时直接返回 ±π/2。
 */
static bool cordic_atan2(motion_fixed_t y, motion_fixed_t x, motion_fixed_t *out)
{
        motion_fixed_t angle = 0;
        motion_fixed_t px, py;

        if (out == 0)
                return false;

        if (x == 0) {
                if (y == 0)
                        return false;           /* 原点角度无定义 */
                *out = y > 0 ? FIXED_PI_2 : -FIXED_PI_2;
                return true;
        }

        /* 归到 x > 0 的半平面 */
        if (x < 0) {
                if (!cordic_atan2(y, -x, &angle))
                        return false;
                if (y >= 0)
                        *out = angle + FIXED_PI;
                else
                        *out = angle - FIXED_PI;
                return true;
        }

        px = x;
        py = y;

        for (u8 i = 0; i < 16; i++) {
                motion_fixed_t sx = px >> i;
                motion_fixed_t sy = py >> i;

                if (py >= 0) {
                        px += sy;
                        py -= sx;
                        angle += cordic_atan[i];
                } else {
                        px -= sy;
                        py += sx;
                        angle -= cordic_atan[i];
                }
        }

        *out = angle;
        return true;
}

bool joint_angle_from_orientation(const motion_fixed_t q[4],
                                  motion_fixed_t *angle_rad)
{
        motion_fixed_t half, doubled;

        if (q == 0 || angle_rad == 0)
                return false;

        /*
         * q 是绕 z 转 θ 的四元数，故 qw = cos(θ/2)、qz = sin(θ/2)，
         * 于是 θ = 2·atan2(qz, qw)。这不是近似 —— 上一步用 θ ≈ 2·qz
         * 才是近似，它在 |θ| > 0.6 rad 时误差超过 2%。
         */
        if (!cordic_atan2(q[3], q[0], &half))
                return false;
        if (fixed_mul2_checked(half, &doubled) != FIXED_OK)
                return false;

        *angle_rad = doubled;
        (void)abs_q;
        return true;
}

bool control_set_frame(control_loop_t *c, wire_t frame)
{
        if (c == 0)
                return false;

        /* WIRE_NONE 表示「相对世界」 */
        c->frame = (frame == WIRE_NONE) ? FRAME_WORLD : frame;

        /* 参照系必须当下有效，否则命令会静默落到一个死句柄上 */
        return base_wire_valid(c->frame);
}

bool joint_control_step(control_loop_t *c, u64 now_ns)
{
        object_state_t st;
        motion_fixed_t angle, error, command;
        motion_event_t ev;

        if (c == 0 || !due(c, now_ns))
                return false;

        if (!object_state(c->wire, &st))
                return false;
        if (!joint_angle_from_orientation(st.orientation, &angle))
                return false;

        if (fixed_sub_checked(c->target, angle, &error) != FIXED_OK)
                return false;
        if (fixed_mul_checked(error, c->gain, &command) != FIXED_OK)
                return false;

        command = clamp(command, c->limit);

        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = 0;
        ev.wire = c->wire;
        ev.frame = c->frame;
        ev.issued_at_ns = now_ns;
        ev.duration_ns = c->period_ns * 2u;
        ev.dof = 6;
        ev.motion[5] = command;                 /* 绕 z 的角速度 */

        c->commands_sent++;
        if (base_apply(now_ns, &ev) != MOTION_OK)
                c->refusals++;

        return true;
}
