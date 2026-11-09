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
