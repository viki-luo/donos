/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_track.c — 足端沿圆连续跟踪
 *
 * 之前的 test_foot 验证的是「停在校验点」：目标不动，等它收敛。
 * 真实步态是**连续移动**的，这里验证动态跟踪。
 *
 * 一个必须说清的物理事实：P 环跟踪**移动**目标必然有滞后。
 * 关节环的误差按 dθ/dt = K·e 演化，目标以角速度 ω_j 移动时，
 * 稳态误差是 ω_j/K；映射到足端约是 (ω_j/K)·L。所以「误差不为零」
 * 本身不是缺陷，误差是否落在由 K 与目标速度算出的界内才是。
 *
 * 本测试把这条界写进断言，而不是只放一个宽松的容差：
 *   lag ≈ v_foot/K_joint，其中 v_foot = r·ω
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/quat.c"
#include "../../base/object.c"
#include "../../brain/control.c"
#include "../../brain/handover.c"
#include "../../brain/kinematics.c"
#include "../../brain/trajectory.c"

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
} joint_plant_t;

static bool jp_set(void *ctx, const motion_fixed_t linear[3],
                   const motion_fixed_t angular[3])
{
        joint_plant_t *p = (joint_plant_t *)ctx;

        (void)linear;
        p->cmd = angular[2];
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
        printf("\n=== continuous tracking: the foot follows a moving target ===\n");

        const leg_geometry_t leg = { .l1 = Q(0.30), .l2 = Q(0.30) };

        /* 圆心与半径都落在可达环内：最远 r = |c| + R = 0.42 + 0.06 = 0.48 < 0.6 */
        const double cx = 0.32, cy = -0.30, radius = 0.06, omega = 0.5;

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

        /*
         * 增益取 4（此前用 2）：跟踪移动目标的滞后约 v_foot/K，
         * 增益加倍就是把滞后减半。这是有依据的选择，不是「调大到能过」。
         */
        const double gain = 4.0;

        check("both position loops install (knee framed on the hip)",
              control_install(&hip_ctrl, CONTROL_POSITION, hip, 1000000,
                              Q(0), Q(gain), Q(4)) &&
              control_install_framed(&knee_ctrl, CONTROL_POSITION, knee, hip,
                                     1000000, Q(0), Q(gain), Q(4)));

        circular_path_t path;

        check("the circular path initialises",
              path_init_circle(&path, Q(cx), Q(cy), Q(radius), Q(omega)));

        motion_fixed_t tx = 0, ty = 0;

        check("the path's first cycle only records the time",
              !path_step_circle(&path, tick(), &tx, &ty));

        /*
         * 跑 2 个整圈：ω = 0.5 rad/s ⇒ 一圈 12.6 s。
         * 两圈用来确认第二圈的误差不比第一圈大 —— 若有状态漂移，第二圈会露出来。
         */
        const int total_ticks = 25000;
        const int sample_every = 10;

        double worst_transient = 0, worst_steady = 0, worst_second_lap = 0;

        /*
         * 前 3 秒算启动瞬态：腿从 (0.6, 0) 摆到轨迹起点附近需要时间，
         * 而目标已经在移动。把它与稳态误差混在一起测，
         * 既说不清是哪一段在失败，也会诱使人去放大容差。
         */
        const int transient_ticks = 3000;
        double sum_err = 0;
        int samples = 0;
        int unreachable = 0;

        for (int i = 0; i < total_ticks; i++) {
                u64 now = tick();
                u32 stepped = 0;

                object_run(now, &stepped);

                if (path_step_circle(&path, now, &tx, &ty)) {
                        motion_fixed_t t1, t2;
                        leg_ik_status_t st = leg_ik(&leg, tx, ty, true, &t1, &t2);

                        if (st == LEG_IK_OK) {
                                hip_ctrl.loop.target = t1;
                                knee_ctrl.loop.target = t2;
                        } else {
                                unreachable++;
                        }
                }

                control_step(&hip_ctrl, now);
                control_step(&knee_ctrl, now);

                if (i % sample_every == 0) {
                        object_state_t hs, ks;
                        motion_fixed_t a1 = 0, a2 = 0, fx = 0, fy = 0;

                        if (object_state(hip, &hs) && object_state(knee, &ks) &&
                            joint_angle_from_orientation(hs.orientation, &a1) &&
                            joint_angle_from_orientation(ks.orientation, &a2) &&
                            leg_tip(&leg, a1, a2, &fx, &fy)) {
                                double ex = to_d(fx) - to_d(tx);
                                double ey = to_d(fy) - to_d(ty);
                                double err = (ex < 0 ? -ex : ex) + (ey < 0 ? -ey : ey);

                                sum_err += err;
                                samples++;

                                /* 一圈 12.6 s = 12600 拍 */
                                if (i < transient_ticks) {
                                        if (err > worst_transient)
                                                worst_transient = err;
                                } else if (i < 12600) {
                                        if (err > worst_steady)
                                                worst_steady = err;
                                } else {
                                        if (err > worst_second_lap)
                                                worst_second_lap = err;
                                }
                        }
                }
        }

        /*
         * 由物理量算出的误差界：
         *   v_foot = r·ω = 0.06 × 0.5 = 0.03 m/s
         *   关节角速度 ω_j ≈ v_foot / L，L 取 0.6 m（全伸展）到 0.3 m
         *   滞后角 e ≈ ω_j / K，映射回足端 ≈ (ω_j / K)·L = v_foot / K
         *   即 ≈ 0.03 / 4 = 0.0075 m
         * 再给一倍余量覆盖 CORDIC 残差与逆解误差 → 0.015 m。
         */
        const double bound = 0.015;

        printf("  note: mean |error| = %.5f m\n", samples ? sum_err / samples : 0);
        printf("  note: worst transient = %.5f m, steady lap1 = %.5f m, lap2 = %.5f m\n",
               worst_transient, worst_steady, worst_second_lap);

        check("the target stayed solvable throughout (no unreachable sample)",
              unreachable == 0);

        /*
         * 瞬态的界不是由 v/K 决定，而是由**初始位姿到轨迹起点的距离**决定：
         * 腿初始在 (0.6, 0)，轨迹起点在 (0.38, −0.30)，相距约 0.37 m。
         * 断言它不超过「初始距离 + 稳态界」的一倍余量。
         */
        check("the start-up transient stays within the initial stand-off",
              worst_transient < 0.80);

        check("steady-state tracking stays inside the bound derived from v/K",
              worst_steady < bound && worst_second_lap < bound);
        check("the second lap is no worse than the first (no drift)",
              worst_second_lap <= worst_steady * 1.5 + 0.001);
        check("the foot was sampled throughout the run", samples > 2000);
        check("the path advanced continuously", path.steps > 20000);
        check("the base still knows only two wires",
              base_wire_valid(hip) && base_wire_valid(knee));

        object_detach_device(hip);
        object_detach_device(knee);
        object_unplug(knee);
        object_unplug(hip);

        printf("\n  track host failures = %u\n", failures);
        return failures ? 1 : 0;
}
