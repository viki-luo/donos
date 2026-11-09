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

fixed_status_t fixed_div_checked(motion_fixed_t a, motion_fixed_t b,
                                 motion_fixed_t *out)
{
        u64 ua, ub;
        u128 num, rem, q;
        bool neg;
        int k;

        if (out == 0)
                return FIXED_E_INVAL;
        if (b == 0)
                return FIXED_E_INVAL;

        /*
         * 取幅值时不能直接取负：-INT64_MIN 会溢出。
         * 用 -(x + 1) + 1 的写法绕开这个边界。
         */
        neg = (a < 0) != (b < 0);
        ua = (u64)(a < 0 ? -(a + 1) : a) + (a < 0 ? 1u : 0u);
        ub = (u64)(b < 0 ? -(b + 1) : b) + (b < 0 ? 1u : 0u);

        /*
         * 计算 (ua << 32) / ub。
         *
         * 只做移位、比较与减法，**不做除法**：一旦写成 __int128 除法，
         * 编译器会调用 __udivti3，而裸机链接里没有这个运行时符号。
         * 移位与比较用 128 位是安全的，那是编译器内联的指令。
         */
        num = ((u128)ua) << 32;
        rem = 0;
        q = 0;

        for (k = 95; k >= 0; k--) {
                rem = (rem << 1) | ((num >> k) & 1u);
                q <<= 1;
                if (rem >= (u128)ub) {
                        rem -= (u128)ub;
                        q |= 1u;
                }
        }

        /* 商超出正半区就是溢出：宁可拒绝，也不返回一个假值。 */
        if (q > (u128)INT64_MAX)
                return FIXED_E_OVERFLOW;

        *out = neg ? -(motion_fixed_t)q : (motion_fixed_t)q;
        return FIXED_OK;
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

fixed_status_t fixed_atan2(motion_fixed_t y, motion_fixed_t x, motion_fixed_t *out)
{
        motion_fixed_t angle = 0;
        motion_fixed_t px, py;

        if (out == 0)
                return FIXED_E_INVAL;

        if (x == 0) {
                if (y == 0)
                        return FIXED_E_INVAL;   /* 原点角度无定义 */
                *out = y > 0 ? FIXED_PI_2 : -FIXED_PI_2;
                return FIXED_OK;
        }

        /*
         * 归到 x > 0 的半平面。
         *
         * 正确恒等式是 atan2(y, x) = π − atan2(y, −x)（x < 0），随后归一化到
         * (−π, π]。早先写成 ±π 加减，模 2π 虽然同余，但**会把 y 的符号翻掉**：
         * atan2(−0.35, −0.30) 得到 +2.279 而不是 −2.279，逆解于是把足端
         * 关于 x 轴镜像。往返测试在一个取样点上抓到了它。
         */
        if (x < 0) {
                if (fixed_atan2(y, -x, &angle) != FIXED_OK)
                        return FIXED_E_INVAL;

                *out = FIXED_PI - angle;

                /* 归一化到 (−π, π] */
                if (*out > FIXED_PI)
                        *out -= FIXED_2PI;
                else if (*out <= -FIXED_PI)
                        *out += FIXED_2PI;

                return FIXED_OK;
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
        return FIXED_OK;
}



/*
 * 整数平方根（向下取整）。牛顿迭代，只在两个 u64 之间做除法，
 * 不产生 128 位除法调用。
 */
u64 fixed_isqrt_u64(u64 v)
{
        u64 x, y;

        if (v == 0)
                return 0;

        /* 初值取 2 的幂：宁可高也不要低，牛顿从上方逼近 */
        x = v;
        y = (x + 1) / 2;
        while (y < x) {
                x = y;
                y = (x + v / x) / 2;
        }
        return x;
}

/*
 * Q32.32 平方根：sqrt(v_raw · 2^32) = isqrt(v_raw) · 2^16。
 * 负数无定义 —— 返回 FIXED_E_INVAL 而不是一个假值。
 */
fixed_status_t fixed_sqrt(motion_fixed_t v, motion_fixed_t *out)
{
        if (out == 0)
                return FIXED_E_INVAL;
        if (v < 0)
                return FIXED_E_INVAL;

        *out = (motion_fixed_t)(fixed_isqrt_u64((u64)v) << 16);
        return FIXED_OK;
}

fixed_status_t fixed_sin_cos(motion_fixed_t radians,
                             motion_fixed_t *sin_out,
                             motion_fixed_t *cos_out)
{
        motion_fixed_t x, x2, x4, term, s, c;
        s64 quadrant, wrapped;
        bool swapped = false;

        if (sin_out == 0 || cos_out == 0)
                return FIXED_E_INVAL;

        /*
         * 归约到 [0, 2π)。除数是编译期常数，编译器用硬件除法或乘倒数，
         * 不产生 libcall。
         */
        wrapped = radians % FIXED_2PI;
        if (wrapped < 0)
                wrapped += FIXED_2PI;

        quadrant = wrapped / FIXED_PI_2;        /* 0..3 */
        x = wrapped - quadrant * FIXED_PI_2;    /* [0, π/2) */

        /*
         * 把 [0, π/2) 折到 [-π/4, π/4]：x > π/4 时用 π/2 - x，并把 sin 与
         * cos 互换（sin(r) = cos(r')，cos(r) = sin(r')）。互换发生在正象限内，
         * 不涉及符号。
         */
        if (x > FIXED_PI_4) {
                x = FIXED_PI_2 - x;
                swapped = true;
        }

        if (fixed_mul_checked(x, x, &x2) != FIXED_OK)
                return FIXED_E_OVERFLOW;
        if (fixed_mul_checked(x2, x2, &x4) != FIXED_OK)
                return FIXED_E_OVERFLOW;

        /* sin = x - x³/6 + x⁵/120 - x⁷/5040 */
        s = x;
        if (fixed_mul_checked(x2, x, &term) != FIXED_OK ||
            fixed_div_checked(term, (motion_fixed_t)6 << 32, &term) != FIXED_OK ||
            fixed_sub_checked(s, term, &s) != FIXED_OK)
                return FIXED_E_OVERFLOW;
        if (fixed_div_checked(x4, (motion_fixed_t)120 << 32, &term) != FIXED_OK ||
            fixed_mul_checked(term, x, &term) != FIXED_OK ||
            fixed_add_checked(s, term, &s) != FIXED_OK)
                return FIXED_E_OVERFLOW;
        if (fixed_mul_checked(x4, x2, &term) != FIXED_OK ||
            fixed_div_checked(term, (motion_fixed_t)5040 << 32, &term) != FIXED_OK ||
            fixed_mul_checked(term, x, &term) != FIXED_OK ||
            fixed_sub_checked(s, term, &s) != FIXED_OK)
                return FIXED_E_OVERFLOW;
        if (fixed_mul_checked(x4, x4, &term) != FIXED_OK ||
            fixed_div_checked(term, (motion_fixed_t)362880 << 32, &term) != FIXED_OK ||
            fixed_mul_checked(term, x, &term) != FIXED_OK ||
            fixed_add_checked(s, term, &s) != FIXED_OK)
                return FIXED_E_OVERFLOW;

        /* cos = 1 - x²/2 + x⁴/24 - x⁶/720 */
        c = (motion_fixed_t)1 << 32;
        if (fixed_div_checked(x2, (motion_fixed_t)2 << 32, &term) != FIXED_OK ||
            fixed_sub_checked(c, term, &c) != FIXED_OK)
                return FIXED_E_OVERFLOW;
        if (fixed_div_checked(x4, (motion_fixed_t)24 << 32, &term) != FIXED_OK ||
            fixed_add_checked(c, term, &c) != FIXED_OK)
                return FIXED_E_OVERFLOW;
        if (fixed_mul_checked(x4, x2, &term) != FIXED_OK ||
            fixed_div_checked(term, (motion_fixed_t)720 << 32, &term) != FIXED_OK ||
            fixed_sub_checked(c, term, &c) != FIXED_OK)
                return FIXED_E_OVERFLOW;
        if (fixed_mul_checked(x4, x4, &term) != FIXED_OK ||
            fixed_div_checked(term, (motion_fixed_t)40320 << 32, &term) != FIXED_OK ||
            fixed_add_checked(c, term, &c) != FIXED_OK)
                return FIXED_E_OVERFLOW;

        if (swapped) {
                motion_fixed_t tmp = s;

                s = c;
                c = tmp;
        }

        /*
         * 旋转 k·π/2：每次 (s, c) → (c, -s)，
         * 因为 sin(θ+π/2) = cos(θ) 且 cos(θ+π/2) = -sin(θ)。
         */
        for (s64 k = 0; k < quadrant; k++) {
                motion_fixed_t tmp = c;

                c = -s;
                s = tmp;
        }

        *sin_out = s;
        *cos_out = c;
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
