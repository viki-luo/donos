/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * quat.c — checked Q32.32 quaternion products and normalisation.
 *
 * Object-side mathematics: the base never touches these functions
 * (make check-purity keeps base/base.c free of attitude vocabulary).
 * Every operation reports failure instead of relying on wraparound.
 */
#include "fixed.h"
#include "quat.h"

#define Q_ONE 4294967296LL             /* 1.0 in Q32.32 */

/* Hamilton product a ⊗ b, component by component, every sum checked. */
static bool q_mul(const motion_fixed_t a[4], const motion_fixed_t b[4],
                  motion_fixed_t out[4])
{
        motion_fixed_t t0, t1, t2, t3;

        /* out0 = a0b0 - a1b1 - a2b2 - a3b3 */
        if (fixed_mul_checked(a[0], b[0], &t0) != FIXED_OK ||
            fixed_mul_checked(a[1], b[1], &t1) != FIXED_OK ||
            fixed_mul_checked(a[2], b[2], &t2) != FIXED_OK ||
            fixed_mul_checked(a[3], b[3], &t3) != FIXED_OK ||
            fixed_sub_checked(t0, t1, &t0) != FIXED_OK ||
            fixed_sub_checked(t0, t2, &t0) != FIXED_OK ||
            fixed_sub_checked(t0, t3, &out[0]) != FIXED_OK)
                return false;

        /* out1 = a0b1 + a1b0 + a2b3 - a3b2 */
        if (fixed_mul_checked(a[0], b[1], &t0) != FIXED_OK ||
            fixed_mul_checked(a[1], b[0], &t1) != FIXED_OK ||
            fixed_mul_checked(a[2], b[3], &t2) != FIXED_OK ||
            fixed_mul_checked(a[3], b[2], &t3) != FIXED_OK ||
            fixed_add_checked(t0, t1, &t0) != FIXED_OK ||
            fixed_add_checked(t0, t2, &t0) != FIXED_OK ||
            fixed_sub_checked(t0, t3, &out[1]) != FIXED_OK)
                return false;

        /* out2 = a0b2 - a1b3 + a2b0 + a3b1 */
        if (fixed_mul_checked(a[0], b[2], &t0) != FIXED_OK ||
            fixed_mul_checked(a[1], b[3], &t1) != FIXED_OK ||
            fixed_mul_checked(a[2], b[0], &t2) != FIXED_OK ||
            fixed_mul_checked(a[3], b[1], &t3) != FIXED_OK ||
            fixed_add_checked(t0, t2, &t0) != FIXED_OK ||
            fixed_add_checked(t0, t3, &t0) != FIXED_OK ||
            fixed_sub_checked(t0, t1, &out[2]) != FIXED_OK)
                return false;

        /* out3 = a0b3 + a1b2 - a2b1 + a3b0 */
        if (fixed_mul_checked(a[0], b[3], &t0) != FIXED_OK ||
            fixed_mul_checked(a[1], b[2], &t1) != FIXED_OK ||
            fixed_mul_checked(a[2], b[1], &t2) != FIXED_OK ||
            fixed_mul_checked(a[3], b[0], &t3) != FIXED_OK ||
            fixed_add_checked(t0, t1, &t0) != FIXED_OK ||
            fixed_add_checked(t0, t3, &t0) != FIXED_OK ||
            fixed_sub_checked(t0, t2, &out[3]) != FIXED_OK)
                return false;

        return true;
}

/*
 * ω ⊗ q for an angular velocity expressed in frame (parent) coordinates.
 *
 * With q mapping body → frame, q̇ = ½ ω ⊗ q.  The left product is the one
 * consistent with object_resolve(), which rotates cmd_motion[3..5] by each
 * frame's orientation when walking the chain: the commanded angular velocity
 * lives in frame coordinates, not body coordinates.
 */
bool quat_angular_product(const motion_fixed_t q[4],
                          const motion_fixed_t omega[3],
                          motion_fixed_t out[4])
{
        const motion_fixed_t w[4] = { 0, omega[0], omega[1], omega[2] };

        return q_mul(w, q, out);
}

/* Floor integer square root of a non-negative u64. No libc, no float. */
static u64 isqrt_u64(u64 v)
{
        u64 res = 0;
        u64 bit = (u64)1 << 62;

        while (bit > v)
                bit >>= 2;
        while (bit != 0) {
                if (v >= res + bit) {
                        v -= res + bit;
                        res = (res >> 1) + bit;
                } else {
                        res >>= 1;
                }
                bit >>= 2;
        }
        return res;
}

/*
 * Normalise in place, preserving every component's sign.
 *
 * A Newton iteration on a Q32.32 scale factor silently mirrored the whole
 * quaternion (q became -q): both are unit rotations, but a pose that flips
 * behind the caller's back is a defect, not a convention.  This version keeps
 * the arithmetic exact and sign-preserving:
 *
 *   sum = |q|² · 2^32  (each component squared through the checked layer)
 *   sqrt(sum) = |q| · 2^16
 *   q' = q / |q| = q · 2^16 / sqrt(sum)
 *
 * Components with |q| ≥ 2^15 are refused outright: such a value is not a
 * pose by any margin, and shifting it left 16 bits would overflow.  The
 * truncating division loses less than 2^-15 of relative length, far inside
 * the 1/1024 unit-norm tolerance the pose validator accepts.
 */
#define QUAT_ABS_MAX (1LL << 45)

bool quat_normalize(motion_fixed_t q[4])
{
        motion_fixed_t sum = 0;
        u64 root;

        for (u8 i = 0; i < 4; i++) {
                motion_fixed_t sq;

                if (q[i] > QUAT_ABS_MAX || q[i] < -QUAT_ABS_MAX)
                        return false;
                if (fixed_mul_checked(q[i], q[i], &sq) != FIXED_OK ||
                    fixed_add_checked(sum, sq, &sum) != FIXED_OK)
                        return false;
        }
        if (sum == 0)
                return false;           /* the caller never passed a pose */

        root = isqrt_u64((u64)sum);
        if (root == 0)
                return false;

        for (u8 i = 0; i < 4; i++) {
                u64 mag = (u64)(q[i] < 0 ? -q[i] : q[i]);
                u64 scaled = (mag << 16) / root;

                if (scaled > (u64)INT64_MAX)
                        return false;
                q[i] = q[i] < 0 ? -(motion_fixed_t)scaled
                                : (motion_fixed_t)scaled;
        }
        return true;
}
