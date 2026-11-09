/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_gait.c — 多关节协调的宿主测试
 *
 * 场景：一条两连杆腿，髋与膝各一条接线，膝的参照系是髋。
 * 步态产生两个关节的目标角，两个位置环各自闭环，大脑用**实测**角度
 * 做正运动学算出足端位置。
 *
 * 要证明的：
 *   1. 两条接线真的各自被驱动，且膝关节的命令参照髋关节（不是世界）。
 *   2. 足端轨迹落在几何可达范围内 —— 这是必然成立的不变量，
 *      越界就说明角度被算错了。
 *   3. 足端真的动了（不是停在一个点）。
 *   4. 基座对「这是一条腿」一无所知：它只看到两条接线。
 *   5. 步态频率可以整体替换，而控制回路与基座都不用改。
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/quat.c"
#include "../../base/object.c"
#include "../../brain/control.c"
#include "../../brain/kinematics.c"
#include "../../brain/handover.c"
#include "../../brain/gait.c"

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

static int near_ratio(motion_fixed_t got, motion_fixed_t want, int permille)
{
        motion_fixed_t diff = got > want ? got - want : want - got;
        motion_fixed_t limit = (want < 0 ? -want : want) / 1000 * permille;

        return diff <= limit;
}

static u64 clk_ns = 1000000;

static u64 tick(void)
{
        clk_ns += 1000000;
        return clk_ns;
}

/* 一个关节的执行机构：角速度一阶跟随命令，角度由角速度积分 */
typedef struct {
        motion_fixed_t cmd;
        motion_fixed_t rate;
        motion_fixed_t angle;
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

        /* 每拍 1 ms：角度增量 = ω × 1e-3 */
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

static const motion_device_ops_t joint_ops = {
        .set_velocity = jp_set,
        .sample = jp_sample,
};

int main(void)
{
        printf("\n=== multi-joint: a leg is two wires, not one object ===\n");

        base_init();
        object_init();

        /* 髋与膝：两条独立接线，膝的命令参照髋 */
        wire_t hip = object_plug(6, 1000000);
        wire_t knee = object_plug(6, 1000000);

        check("the hip wire plugs", hip != WIRE_NONE);
        check("the knee wire plugs", knee != WIRE_NONE);

        joint_plant_t hip_plant = { 0 };
        joint_plant_t knee_plant = { 0 };

        check("the hip plant attaches",
              object_attach_device(hip, &joint_ops, &hip_plant));
        check("the knee plant attaches",
              object_attach_device(knee, &joint_ops, &knee_plant));

        /* 两条位置环；膝关节的命令相对髋关节 */
        control_source_t hip_ctrl = { 0 };
        control_source_t knee_ctrl = { 0 };

        check("the hip position loop installs",
              control_install(&hip_ctrl, CONTROL_POSITION, hip, 1000000,
                              Q(0), Q(2), Q(2)));
        check("the knee position loop installs, framed on the hip",
              control_install_framed(&knee_ctrl, CONTROL_POSITION, knee, hip,
                                     1000000, Q(0), Q(2), Q(2)));

        /* 步态：髋摆 0.3 rad，膝摆 0.5 rad，相位差 π/2 */
        gait_t gait;
        check("the gait initialises",
              gait_init(&gait, 1000000, Q(0.5)));
        check("the hip joint is added",
              gait_add_joint(&gait, hip, Q(0), Q(0.3), Q(0)));
        check("the knee joint is added with a phase offset",
              gait_add_joint(&gait, knee, Q(0), Q(0.5), Q(1.5707963)));

        motion_fixed_t targets[GAIT_JOINTS_MAX] = { 0 };

        /* 第一拍只记时刻 */
        check("the gait's first cycle only records the time",
              !gait_step(&gait, tick(), targets, GAIT_JOINTS_MAX));

        /* 跑 3 秒：足够让两个关节跟上一个完整周期 */
        const leg_geometry_t leg = { .l1 = Q(0.3), .l2 = Q(0.3) };
        motion_fixed_t min_x = 0, max_x = 0, min_y = 0, max_y = 0;
        bool all_reachable = true;
        u32 samples = 0;

        for (int i = 0; i < 3000; i++) {
                u64 now = tick();
                u32 stepped = 0;

                object_run(now, &stepped);

                if (gait_step(&gait, now, targets, GAIT_JOINTS_MAX)) {
                        hip_ctrl.loop.target = targets[0];
                        knee_ctrl.loop.target = targets[1];
                }

                control_step(&hip_ctrl, now);
                control_step(&knee_ctrl, now);

                /* 每 10 拍用实测角度算一次足端 */
                if (i % 10 == 0) {
                        object_state_t hs, ks;
                        motion_fixed_t a1 = 0, a2 = 0, x = 0, y = 0;

                        if (object_state(hip, &hs) && object_state(knee, &ks) &&
                            joint_angle_from_orientation(hs.orientation, &a1) &&
                            joint_angle_from_orientation(ks.orientation, &a2) &&
                            leg_tip(&leg, a1, a2, &x, &y)) {
                                if (!leg_reachable(&leg, x, y))
                                        all_reachable = false;

                                if (samples == 0 || x < min_x) min_x = x;
                                if (samples == 0 || x > max_x) max_x = x;
                                if (samples == 0 || y < min_y) min_y = y;
                                if (samples == 0 || y > max_y) max_y = y;
                                samples++;
                        }
                }
        }

        check("both joints were actually driven",
              hip_plant.sets > 100 && knee_plant.sets > 100);
        check("the foot stayed inside the geometrically reachable ring",
              all_reachable && samples > 100);
        check("the foot actually travelled in x",
              (max_x - min_x) > Q(0.02));
        check("the foot actually travelled in y",
              (max_y - min_y) > Q(0.02));

        /* 两个关节都跟上了各自的目标（有相位差，所以角度不同） */
        {
                object_state_t hs, ks;
                motion_fixed_t a1 = 0, a2 = 0;

                object_state(hip, &hs);
                object_state(knee, &ks);
                joint_angle_from_orientation(hs.orientation, &a1);
                joint_angle_from_orientation(ks.orientation, &a2);

                check("the hip tracked its trajectory",
                      a1 > -Q(0.35) && a1 < Q(0.35));
                check("the knee tracked its trajectory",
                      a2 > -Q(0.55) && a2 < Q(0.55));
                check("the two joints are not moving in lockstep",
                      !near_ratio(a1, a2, 50));
        }

        /* 替换步态频率：控制回路与基座都不用改 */
        {
                gait_t faster;

                check("a second gait is prepared",
                      gait_init(&faster, 1000000, Q(2)));
                check("it carries the same two joints",
                      gait_add_joint(&faster, hip, Q(0), Q(0.3), Q(0)) &&
                      gait_add_joint(&faster, knee, Q(0), Q(0.5), Q(1.5707963)));

                gait = faster;          /* 整体替换相位来源 */

                u32 before = gait.steps;

                gait_step(&gait, tick(), targets, GAIT_JOINTS_MAX);
                gait_step(&gait, tick(), targets, GAIT_JOINTS_MAX);
                check("the new gait advances", gait.steps > before);
                check("the base still knows only two wires",
                      base_wire_valid(hip) && base_wire_valid(knee));
        }

        object_detach_device(hip);
        object_detach_device(knee);
        object_unplug(knee);
        object_unplug(hip);

        printf("\n  gait host failures = %u\n", failures);
        return failures ? 1 : 0;
}
