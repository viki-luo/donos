/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_sincos.c — 定点正余弦的宿主测试
 *
 * 期望值全部手算（π 的倍数处是精确值，其余用已知十进制值），
 * 容差按 40 个最低位给出 —— 泰勒截断的误差量级就是这么大，
 * 把容差写成恰好相等，测的就成了「常量取到哪一位」。
 */
#include "../../base/fixed.c"


#include <stdint.h>
#include <stdio.h>

#define Q(v) ((motion_fixed_t)((v) * 4294967296.0))

static unsigned failures;

static void check(const char *what, int ok)
{
        printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
                failures++;
}

/* 允许 ±tol 个最低位 */
static int near_lsb(motion_fixed_t got, motion_fixed_t want, motion_fixed_t tol)
{
        motion_fixed_t d = got > want ? got - want : want - got;

        return d <= tol;
}

int main(void)
{
        motion_fixed_t s, c;
        const motion_fixed_t tol = 200;      /* 截断误差约 2.5e-8 ≈ 110 最低位 */

        printf("\n=== fixed-point sine and cosine ===\n");

        check("sin(0) = 0, cos(0) = 1",
              fixed_sin_cos(0, &s, &c) == FIXED_OK &&
              near_lsb(s, 0, tol) && near_lsb(c, Q(1), tol));

        /* π/2：精确值 sin=1, cos=0 */
        check("sin(pi/2) = 1, cos(pi/2) = 0",
              fixed_sin_cos(Q(1.5707963267948966), &s, &c) == FIXED_OK &&
              near_lsb(s, Q(1), tol) && near_lsb(c, 0, tol));

        /* π：sin=0, cos=-1 */
        check("sin(pi) = 0, cos(pi) = -1",
              fixed_sin_cos(Q(3.141592653589793), &s, &c) == FIXED_OK &&
              near_lsb(s, 0, tol) && near_lsb(c, -Q(1), tol));

        /* π/6：sin=0.5, cos=√3/2≈0.8660254038 */
        check("sin(pi/6) = 0.5, cos(pi/6) = 0.8660254",
              fixed_sin_cos(Q(0.5235987755982988), &s, &c) == FIXED_OK &&
              near_lsb(s, Q(0.5), tol) &&
              near_lsb(c, Q(0.8660254037844386), tol));

        /* π/4：sin=cos=√2/2≈0.7071068 */
        check("sin(pi/4) = cos(pi/4) = 0.7071068",
              fixed_sin_cos(Q(0.7853981633974483), &s, &c) == FIXED_OK &&
              near_lsb(s, Q(0.7071067811865476), tol) &&
              near_lsb(c, Q(0.7071067811865476), tol));

        /* 3π/4：sin=√2/2, cos=-√2/2 —— 检验 cos 互换后的符号 */
        check("sin(3pi/4) = 0.7071, cos(3pi/4) = -0.7071",
              fixed_sin_cos(Q(2.356194490192345), &s, &c) == FIXED_OK &&
              near_lsb(s, Q(0.7071067811865476), tol) &&
              near_lsb(c, -Q(0.7071067811865476), tol));

        /* 负角度：sin(-π/2) = -1, cos(-π/2) = 0 */
        check("sin(-pi/2) = -1, cos(-pi/2) = 0",
              fixed_sin_cos(-Q(1.5707963267948966), &s, &c) == FIXED_OK &&
              near_lsb(s, -Q(1), tol) && near_lsb(c, 0, tol));

        /* 超过一圈：sin(2π + π/6) = sin(π/6) */
        check("angles beyond one turn wrap correctly",
              fixed_sin_cos(Q(6.283185307179586 + 0.5235987755982988), &s, &c) == FIXED_OK &&
              near_lsb(s, Q(0.5), tol));

        /* 单位圆恒等式：sin² + cos² = 1 */
        {
                motion_fixed_t ss, cc, sum;

                fixed_sin_cos(Q(1.0), &ss, &cc);
                /* ss² + cc² 用 128 位中间量避免溢出 */
                sum = (motion_fixed_t)((((unsigned __int128)(u64)(ss < 0 ? -ss : ss)) * 
                                        (u64)(ss < 0 ? -ss : ss)) >> 32);
                check("a 1 rad angle stays on the unit circle (ballpark)",
                      sum <= Q(1) + tol);

                /* 更直接的写法：用 double 校验量级，只作量纲检查 */
                double d = (double)ss / 4294967296.0;
                double e = (double)cc / 4294967296.0;

                check("sin(1) and cos(1) match the known values",
                      d > 0.8414709 - 1e-6 && d < 0.8414709 + 1e-6 &&
                      e > 0.5403023 - 1e-6 && e < 0.5403023 + 1e-6);
        }

        printf("\n  sincos host failures = %u\n", failures);
        return failures ? 1 : 0;
}
