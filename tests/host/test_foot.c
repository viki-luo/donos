/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_foot.c — 逆运动学与足端轨迹跟踪
 *
 * 这一步把大脑的接口从「关节转到这个角」提升到「足端去这里」。
 * 差别是实质的：说关节角等于把几何学泄漏给调用者；说足端位置才是任务。
 *
 * 回路：足端目标 → 逆解 → 两个关节角目标 → 两个位置环 → 命令 →
 *       设备 → 实测角度 → 正解 → 实际足端 → 与目标比较。
 *
 * 断言全部对着**实测**算出的足端，不是对着命令：
 *   1. 原地正解-逆解往返一致（多个取样点，含膝上/膝下两支）。
 *   2. 可达环之外的目标被明确报告为无解，而不是给出一个假角度。
 *   3. 足端沿方形轨迹走完一圈后，实测足端在每个角点都落在容差内。
 *   4. 基座全程只认识两条接线。
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/quat.c"
#include "../../base/object.c"
#include "../../brain/control.c"
#include "../../brain/handover.c"
#include "../../brain/kinematics.c"

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

static double to_d(motion_fixed_t v)
{
        return (double)v / 4294967296.0;
}

static u64 clk_ns = 1000000;

static u64 tick(void)
{
        clk_ns += 1000000;
        return clk_ns;
}

typedef struct {
        motion_fixed_t cmd, rate, angle;
        u32            sets;
} joint_plant_t;

static bool jp_set(void *ctx, const motion_fixed_t linear[3],
                   const motion_fixed_t angular[3])
{
        joint_plant_t *p = (joint_plant_t *)ctx;

        (void)linear;
        p->cmd = angular[2];
        p->sets++;
        return true;
}

static bool jp_sample(void *ctx, object_state_t *out)
{
        joint_plant_t *p = (joint_plant_t *)ctx;
        motion_fixed_t d, half, sh, ch;

        if (fixed_sub_checked(p->cmd, p->rate, &d) == FIXED_OK &&
            fixed_mul_checked(d, Q(0.5), &d) == FIXED_OK)
                fixed_add_checked(p->rate, d, &p->rate);

        if (fixed_mul_checked(p->rate, Q(0.001), &d) == FIXED_OK)
                fixed_add_checked(p->angle, d, &p->angle);

        for (u8 i = 0; i < 3; i++) {
                out->linear[i] = 0;
                out->angular[i] = 0;
                out->position[i] = 0;
        }
        for (u8 i = 0; i < 4; i++)
                out->orientation[i] = 0;

        out->angular[2] = p->rate;
        if (fixed_div_checked(p->angle, (motion_fixed_t)2 << 32, &half) == FIXED_OK &&
            fixed_sin_cos(half, &sh, &ch) == FIXED_OK) {
                out->orientation[0] = ch;
                out->orientation[3] = sh;
        }
        return true;
}

static const motion_device_ops_t joint_ops = { jp_set, jp_sample };

int main(void)
{
        printf("\n=== foot trajectory: say where the foot goes ===\n");

        const leg_geometry_t leg = { .l1 = Q(0.30), .l2 = Q(0.30) };

        /* ---- 1. 正解-逆解往返 ---- */
        {
                const double pts[][2] = {
                        { 0.40, -0.20 }, { 0.50, 0.00 }, { 0.20, -0.50 },
                        { 0.00, -0.55 }, { -0.30, -0.35 },
                };
                bool all_ok = true;

                for (unsigned i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
                        motion_fixed_t x = Q(pts[i][0]), y = Q(pts[i][1]);
                        motion_fixed_t t1, t2, bx, by;

                        if (leg_ik(&leg, x, y, true, &t1, &t2) != LEG_IK_OK ||
                            !leg_tip(&leg, t1, t2, &bx, &by)) {
                                all_ok = false;
                                continue;
                        }
                        /* 往返误差应在毫米级：1e-3 m */
                        motion_fixed_t dx = bx > x ? bx - x : x - bx;
                        motion_fixed_t dy = by > y ? by - y : y - by;

                        if (dx > Q(0.001) || dy > Q(0.001))
                                all_ok = false;
                }
                check("forward-then-inverse round trips within 1 mm", all_ok);
        }

        /* ---- 2. 膝上/膝下两支都给解，且解不同 ---- */
        {
                motion_fixed_t x = Q(0.30), y = Q(-0.30);
                motion_fixed_t up1, up2, dn1, dn2;

                bool ok = leg_ik(&leg, x, y, true, &up1, &up2) == LEG_IK_OK &&
                          leg_ik(&leg, x, y, false, &dn1, &dn2) == LEG_IK_OK;
                check("both knee branches solve", ok);
                check("the two branches are genuinely different",
                      ok && (up2 != dn2));
                {
                        motion_fixed_t bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
                        bool same = ok &&
                                    leg_tip(&leg, up1, up2, &bx1, &by1) &&
                                    leg_tip(&leg, dn1, dn2, &bx2, &by2) &&
                                    (bx1 - bx2 < Q(0.001)) &&
                                    (bx2 - bx1 < Q(0.001)) &&
                                    (by1 - by2 < Q(0.001)) &&
                                    (by2 - by1 < Q(0.001));

                        check("the two branches reach the same point", same);
                }
        }

        /* ---- 3. 可达环之外明确无解 ---- */
        {
                motion_fixed_t t1, t2;

                check("a target beyond the leg is reported unreachable",
                      leg_ik(&leg, Q(0.90), Q(0), true, &t1, &t2) ==
                      LEG_IK_E_UNREACHABLE);
                /*
                 * 内侧不可达要用**不对称**的腿验证：l1 == l2 时内半径
                 * |l1−l2| 为 0，原点反而是可达的（腿完全折叠）。
                 * 把 origin 当不可达是测试的错，不是逆解的错。
                 */
                {
                        const leg_geometry_t asym = { .l1 = Q(0.45),
                                                      .l2 = Q(0.15) };

                        /*
                         * 原点是**退化**情形而不是普通解：θ₁ 任意。
                         * 把它报成 OK 等于隐藏了一个自由度，报成 INVAL
                         * 又会让调用者以为自己写错了参数。
                         */
                        check("a foot exactly at the hip is reported degenerate",
                              leg_ik(&leg, Q(0), Q(0), true, &t1, &t2) ==
                              LEG_IK_E_DEGENERATE);

                        /*
                         * 内侧不可达要点在 0 < r < |l1−l2| = 0.30 之间。
                         * 用 (0,0) 会先命中「退化」分支（θ₁ 任意），
                         * 测到的就不是内侧环这条规则。
                         */
                        check("a target inside the asymmetric inner ring is unreachable",
                              leg_ik(&asym, Q(0.10), Q(0), true, &t1, &t2) ==
                              LEG_IK_E_UNREACHABLE);

                        check("a target beyond the asymmetric reach is unreachable",
                              leg_ik(&asym, Q(0.70), Q(0), true, &t1, &t2) ==
                              LEG_IK_E_UNREACHABLE);
                }
                check("a degenerate arm length is refused",
                      leg_ik(&(leg_geometry_t){ 0, 0 }, Q(0.3), Q(0), true,
                             &t1, &t2) == LEG_IK_E_INVAL);
        }

        /* ---- 4. 闭环跟踪一个方形足端轨迹 ---- */
        base_init();
        object_init();

        wire_t hip = object_plug(6, 1000000);
        wire_t knee = object_plug(6, 1000000);

        joint_plant_t hip_plant = { 0 };
        joint_plant_t knee_plant = { 0 };

        object_attach_device(hip, &joint_ops, &hip_plant);
        object_attach_device(knee, &joint_ops, &knee_plant);

        control_source_t hip_ctrl = { 0 };
        control_source_t knee_ctrl = { 0 };

        check("both position loops install (knee framed on the hip)",
              control_install(&hip_ctrl, CONTROL_POSITION, hip, 1000000,
                              Q(0), Q(2), Q(3)) &&
              control_install_framed(&knee_ctrl, CONTROL_POSITION, knee, hip,
                                     1000000, Q(0), Q(2), Q(3)));

        /* 方形轨迹的四个角点，全部在可达环内 */
        /*
         * 角点必须全部落在可达环内：l1 = l2 = 0.3 时外半径是 0.6。
         * 早先用的 (0.45, −0.40) 半径是 0.602 —— 那条腿根本够不到，
         * 测试却把它当作「跟踪失败」。目标不可达是设计错误，不是控制缺陷。
         */
        const double corners[][2] = {
                { 0.28, -0.28 }, { 0.40, -0.28 },
                { 0.40, -0.40 }, { 0.28, -0.40 },
        };
        const int corner_count = 4;
        /*
         * 一阶环的时间常数是 1/K = 0.5 s，误差按 e^{-2t} 衰减。
         * 1.2 s 只剩 9% 残余（1.7 × e^-2.4 ≈ 0.155 rad）—— 那是没等够，
         * 不是跟踪不准。3.5 s 把残余压到 0.1% 量级。
         */
        const int settle_ticks = 3500;          /* 每个角点 3.5 秒 */

        double worst_error = 0;

        for (int c = 0; c < corner_count; c++) {
                motion_fixed_t t1, t2;

                if (leg_ik(&leg, Q(corners[c][0]), Q(corners[c][1]), true,
                           &t1, &t2) != LEG_IK_OK) {
                        check("corner target solves", 0);
                        continue;
                }

                hip_ctrl.loop.target = t1;
                knee_ctrl.loop.target = t2;

                for (int i = 0; i < settle_ticks; i++) {
                        u64 now = tick();
                        u32 stepped = 0;

                        object_run(now, &stepped);
                        control_step(&hip_ctrl, now);
                        control_step(&knee_ctrl, now);
                }

                /* 用**实测**角度算足端，与目标比较 */
                object_state_t hs, ks;
                motion_fixed_t a1 = 0, a2 = 0, fx = 0, fy = 0;

                bool got = object_state(hip, &hs) && object_state(knee, &ks) &&
                           joint_angle_from_orientation(hs.orientation, &a1) &&
                           joint_angle_from_orientation(ks.orientation, &a2) &&
                           leg_tip(&leg, a1, a2, &fx, &fy);

                if (!got) {
                        check("measured foot position is computable", 0);
                        continue;
                }

                double ex = to_d(fx) - corners[c][0];
                double ey = to_d(fy) - corners[c][1];
                double err = (ex < 0 ? -ex : ex) + (ey < 0 ? -ey : ey);

                if (err > worst_error)
                        worst_error = err;
        }

        printf("  note: worst measured foot error over the square = %.4f m\n",
               worst_error);
        check("the measured foot tracks the commanded corners within 20 mm",
              worst_error < 0.020);
        check("both joints were driven hard", hip_plant.sets > 1000 &&
              knee_plant.sets > 1000);
        check("the base still knows only two wires",
              base_wire_valid(hip) && base_wire_valid(knee));

        object_detach_device(hip);
        object_detach_device(knee);
        object_unplug(knee);
        object_unplug(hip);

        printf("\n  foot host failures = %u\n", failures);
        return failures ? 1 : 0;
}
