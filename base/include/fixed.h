/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fixed.h — checked Q32.32 arithmetic for the object side
 *
 * Every operation reports overflow instead of relying on signed wraparound.
 * The base itself does not use this layer: world arithmetic belongs to objects.
 */
#ifndef DONOS_FIXED_H
#define DONOS_FIXED_H

#include "base.h"

typedef enum {
        FIXED_OK = 0,
        FIXED_E_INVAL = -1,
        FIXED_E_OVERFLOW = -2,
} fixed_status_t;

fixed_status_t fixed_add_checked(motion_fixed_t a, motion_fixed_t b,
                                 motion_fixed_t *out);
fixed_status_t fixed_sub_checked(motion_fixed_t a, motion_fixed_t b,
                                 motion_fixed_t *out);
fixed_status_t fixed_mul2_checked(motion_fixed_t value,
                                  motion_fixed_t *out);
fixed_status_t fixed_mul_checked(motion_fixed_t a, motion_fixed_t b,
                                 motion_fixed_t *out);

/*
 * Q32.32 除法：out = a / b，带溢出检查。
 *
 * 为什么不用 (a << 32) / b：a 稍大就会溢出 int64，而改用 __int128
 * 分子会让编译器调用 __divti3 —— 裸机链接里没有这个运行时符号。
 * 实现用 64 轮移位相减的长除法，只做比较与减法，不产生任何除法调用。
 *
 * b == 0 返回 FIXED_E_INVAL；商超出 int64 返回 FIXED_E_OVERFLOW。
 */
fixed_status_t fixed_div_checked(motion_fixed_t a, motion_fixed_t b,
                                 motion_fixed_t *out);

/*
 * Q32.32 正余弦：sin(x) 与 cos(x)，x 为弧度。
 *
 * 用泰勒展开加象限归约：先把角度折到 [-π/4, π/4]，再算
 *   sin = x - x³/6 + x⁵/120 - x⁷/5040
 *   cos = 1 - x²/2 + x⁴/24 - x⁶/720
 * 截断误差约 1e-8，在 Q32.32 上约 40 个最低位。
 *
 * 存在的理由：关节的绝对角度必须表达成姿态四元数
 * q = (cos(θ/2), 0, 0, sin(θ/2))，而姿态是 object_state 规定的旋转表示
 * —— 把弧度塞进 position[]（那里按标准是**米**）是单位违规。
 */
fixed_status_t fixed_sin_cos(motion_fixed_t radians,
                             motion_fixed_t *sin_out,
                             motion_fixed_t *cos_out);

/*
 * Integrate one Q32.32 velocity component over period_ns.
 *
 * Exact integer model:
 *   numerator = velocity * period_ns + carry
 *   delta     = numerator / 1,000,000,000
 *   new_carry = numerator % 1,000,000,000
 *
 * The implementation avoids both signed-overflow UB and compiler 128-bit
 * division helpers. carry must remain strictly inside (-1e9, 1e9).
 */
fixed_status_t fixed_velocity_step_checked(motion_fixed_t velocity,
                                           u64 period_ns,
                                           motion_fixed_t carry,
                                           motion_fixed_t *delta,
                                           motion_fixed_t *new_carry);

#endif /* DONOS_FIXED_H */
