/* SPDX-License-Identifier: GPL-2.0-only */
/* fixed.c — checked Q32.32 arithmetic */
#include "fixed.h"

#define NS_PER_SECOND 1000000000LL

static bool s64_add_overflow(s64 a, s64 b, s64 *out)
{
        if (b > 0 && a > INT64_MAX - b)
                return true;
        if (b < 0 && a < INT64_MIN - b)
                return true;
        *out = a + b;
        return false;
}

static bool s64_sub_overflow(s64 a, s64 b, s64 *out)
{
        if (b > 0 && a < INT64_MIN + b)
                return true;
        if (b < 0 && a > INT64_MAX + b)
                return true;
        *out = a - b;
        return false;
}

static bool s64_mul_nonnegative_overflow(s64 a, u64 b, s64 *out)
{
        if (a > 0 && b > (u64)INT64_MAX / (u64)a)
                return true;
        if (a < 0) {
                u64 magnitude = (u64)(-(a + 1)) + 1u;
                u64 limit = (u64)INT64_MAX + 1u;

                if (b != 0 && magnitude > limit / b)
                        return true;
        }

        *out = a * (s64)b;
        return false;
}

fixed_status_t fixed_add_checked(motion_fixed_t a, motion_fixed_t b,
                                 motion_fixed_t *out)
{
        if (out == 0)
                return FIXED_E_INVAL;
        return s64_add_overflow(a, b, out) ? FIXED_E_OVERFLOW : FIXED_OK;
}

fixed_status_t fixed_sub_checked(motion_fixed_t a, motion_fixed_t b,
                                 motion_fixed_t *out)
{
        if (out == 0)
                return FIXED_E_INVAL;
        return s64_sub_overflow(a, b, out) ? FIXED_E_OVERFLOW : FIXED_OK;
}

fixed_status_t fixed_mul2_checked(motion_fixed_t value,
                                  motion_fixed_t *out)
{
        return fixed_add_checked(value, value, out);
}

fixed_status_t fixed_mul_checked(motion_fixed_t a, motion_fixed_t b,
                                 motion_fixed_t *out)
{
        __int128 product;
        __int128 rounded;
        const __int128 half = (__int128)1 << 31;

        if (out == 0)
                return FIXED_E_INVAL;

        product = (__int128)a * (__int128)b;
        if (product >= 0)
                rounded = (product + half) >> 32;
        else {
                unsigned __int128 magnitude =
                        (unsigned __int128)(-(product + 1)) + 1u;
                rounded = -(__int128)((magnitude + (unsigned __int128)half) >> 32);
        }

        if (rounded > INT64_MAX || rounded < INT64_MIN)
                return FIXED_E_OVERFLOW;

        *out = (motion_fixed_t)rounded;
        return FIXED_OK;
}

fixed_status_t fixed_velocity_step_checked(motion_fixed_t velocity,
                                           u64 period_ns,
                                           motion_fixed_t carry,
                                           motion_fixed_t *delta,
                                           motion_fixed_t *new_carry)
{
        motion_fixed_t whole;
        motion_fixed_t remainder;
        motion_fixed_t whole_delta;
        motion_fixed_t fractional_num;
        motion_fixed_t fractional_delta;
        motion_fixed_t result;

        if (delta == 0 || new_carry == 0)
                return FIXED_E_INVAL;
        if (period_ns > 1000000000ULL)
                return FIXED_E_INVAL;
        if (carry <= -NS_PER_SECOND || carry >= NS_PER_SECOND)
                return FIXED_E_INVAL;

        /*
         * velocity = whole * 1e9 + remainder.  This decomposition lets the
         * only potentially wide product be checked before it is evaluated:
         *
         * velocity * period / 1e9
         *   = whole * period + remainder * period / 1e9.
         *
         * |remainder| < 1e9 and period <= 1e9, so remainder * period plus
         * carry always fits signed 64 bits. No 128-bit division is required.
         */
        whole = velocity / NS_PER_SECOND;
        remainder = velocity % NS_PER_SECOND;

        if (s64_mul_nonnegative_overflow(whole, period_ns, &whole_delta))
                return FIXED_E_OVERFLOW;

        /* Proved safe by the bounds above. */
        fractional_num = remainder * (motion_fixed_t)period_ns + carry;
        fractional_delta = fractional_num / NS_PER_SECOND;

        if (s64_add_overflow(whole_delta, fractional_delta, &result))
                return FIXED_E_OVERFLOW;

        *delta = result;
        *new_carry = fractional_num % NS_PER_SECOND;
        return FIXED_OK;
}
