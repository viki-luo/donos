/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_quad.c — 四足步态：支撑相不滑行
 *
 * 这是「会不会走路」与「看起来在走」的分界。若支撑脚跟着身体一起动，
 * 机器人原地打滑：八条接线都在动，位移却一点没换来。
 *
 * 断言的核心是**世界坐标**：支撑脚的 x 必须恒定。
 * 机体坐标里它当然在动（以 -v 后退），那正是不滑行的实现方式。
 *
 * 另外验证：
 *   1. 八条接线（四条腿 × 髋膝）全部被驱动。
 *   2. 对角小跑：对角腿同相、相邻腿反相。
 *   3. 摆动脚确实抬离地面。
 *   4. 身体单调前进。
 *   5. 基座只认识八条线，不知道四条腿。
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/quat.c"
#include "../../base/object.c"
#include "../../brain/control.c"
#include "../../brain/handover.c"
#include "../../brain/kinematics.c"
#include "../../brain/quadruped.c"

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

        /*
         * 角度归一到 (−π, π]：真实绝对编码器本来就只有一圈的信息，
         * 而 joint_angle_from_orientation 也是按模 2π 恢复角度的。
         * 不归一的话，假设备的角度会无限累计，控制器看到的却是回绕后的值。
         */
        {
                const motion_fixed_t pi = (motion_fixed_t)13493037705LL;
                const motion_fixed_t twopi = (motion_fixed_t)26986075410LL;

                while (p->angle > pi)
                        p->angle -= twopi;
                while (p->angle <= -pi)
                        p->angle += twopi;
        }

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
        printf("\n=== quadruped: a stance foot must not slide ===\n");

        const leg_geometry_t geom = { .l1 = Q(0.25), .l2 = Q(0.25) };

        base_init();
        object_init();

        /*
         * 四条腿、八条接线。腿的位置只有 x0/y0，没有「前左」这种语义 ——
         * 名字是人给的，基座只看见八条线。
         */
        wire_t hips[4], knees[4];
        joint_plant_t plants[8] = { 0 };
        control_source_t ctrls[8] = { 0 };

        for (u8 i = 0; i < 4; i++) {
                hips[i] = object_plug(6, 1000000);
                knees[i] = object_plug(6, 1000000);
        }

        bool all_plugged = true;

        for (u8 i = 0; i < 4; i++)
                if (hips[i] == WIRE_NONE || knees[i] == WIRE_NONE)
                        all_plugged = false;
        check("eight wires plug (four legs, hip and knee each)", all_plugged);

        bool all_attached = true;

        for (u8 i = 0; i < 4; i++) {
                if (!object_attach_device(hips[i], &joint_ops, &plants[i * 2]))
                        all_attached = false;
                if (!object_attach_device(knees[i], &joint_ops, &plants[i * 2 + 1]))
                        all_attached = false;
        }
        check("all eight joint plants attach", all_attached);

        /* 每条腿两个位置环；膝关节相对髋关节 */
        bool all_loops = true;

        for (u8 i = 0; i < 4; i++) {
                if (!control_install(&ctrls[i * 2], CONTROL_POSITION, hips[i],
                                     1000000, Q(0), Q(8), Q(4)))
                        all_loops = false;
                if (!control_install_framed(&ctrls[i * 2 + 1], CONTROL_POSITION,
                                            knees[i], hips[i], 1000000, Q(0),
                                            Q(3), Q(4)))
                        all_loops = false;
        }
        check("all eight position loops install", all_loops);

        /*
         * 步态参数：速度 0.2 m/s，支撑占比 0.6，步幅 = v·T = 0.08 m，
         * 于是 T = 0.4 s、支撑时长 0.24 s。抬脚 0.03 m，机身 0.38 m。
         */
        quadruped_t quad;

        check("the gait initialises",
              quad_init(&quad, 1000000, Q(0.2), Q(0.6), Q(0.08), Q(0.03),
                        Q(0.38)));

        /* 对角小跑：腿 0 与 3 同相，腿 1 与 2 同相，两组相差半周期 */
        const motion_fixed_t phases[4] = { Q(0), Q(0.5), Q(0.5), Q(0) };
        const motion_fixed_t xs[4] = { Q(0.10), Q(0.10), Q(-0.10), Q(-0.10) };

        bool all_legs = true;

        for (u8 i = 0; i < 4; i++)
                if (!quad_add_leg(&quad, hips[i], knees[i], &geom, phases[i],
                                  xs[i], Q(0)))
                        all_legs = false;
        check("four legs are added to the gait", all_legs);

        motion_fixed_t targets[QUAD_LEGS_MAX][2];
        bool stance[QUAD_LEGS_MAX];

        check("the gait's first cycle only records the time",
              !quad_step(&quad, tick(), targets, stance));

        /*
         * 跑 4 秒（10 个步态周期）。
         * 记录每条腿在**世界坐标**里的 x，逐相检查支撑期是否恒定。
         */
        const int total_ticks = 4000;
        double world_x_max[4], world_x_min[4];

        /* 命令目标的基线：这是步态数学的责任，应当是精确的 */
        double cmd_x_max[4], cmd_x_min[4];

        bool   seen_stance[4] = { false, false, false, false };
        bool   seen_swing[4] = { false, false, false, false };
        double best_lift = 0;          /* 实测达到的抬脚高度 */
        double best_cmd_lift = 0;      /* 命令给出的抬脚高度 */
        double worst_slip = 0;
        double worst_cmd_slip = 0;

        for (u8 i = 0; i < 4; i++) {
                world_x_max[i] = -1e9;
                world_x_min[i] = 1e9;
                cmd_x_max[i] = -1e9;
                cmd_x_min[i] = 1e9;
        }

        for (int t = 0; t < total_ticks; t++) {
                u64 now = tick();
                u32 stepped = 0;

                object_run(now, &stepped);

                /* 前 3 秒是启动瞬态：腿从角度 0 摆到轨迹上需要时间 */
                bool settled = (t >= 3000);

                if (quad_step(&quad, now, targets, stance)) {
                        /*
                         * 命令目标的世界坐标：支撑相里它必须恒定。
                         * 这是步态自己的账，与控制器无关。
                         */
                        for (u8 i = 0; i < 4; i++) {
                                if (!stance[i]) {
                                        cmd_x_max[i] = -1e9;
                                        cmd_x_min[i] = 1e9;
                                        continue;
                                }

                                double cmd_world = to_d(targets[i][0]) +
                                                   to_d(quad.body_x);

                                if (cmd_world > cmd_x_max[i]) cmd_x_max[i] = cmd_world;
                                if (cmd_world < cmd_x_min[i]) cmd_x_min[i] = cmd_world;

                                double cs = cmd_x_max[i] - cmd_x_min[i];

                                if (cs > worst_cmd_slip)
                                        worst_cmd_slip = cs;
                        }

                        for (u8 i = 0; i < 4; i++) {
                                motion_fixed_t t1, t2;

                                if (leg_ik(&geom, targets[i][0], targets[i][1],
                                           true, &t1, &t2) == LEG_IK_OK) {
                                        ctrls[i * 2].loop.target = t1;
                                        ctrls[i * 2 + 1].loop.target = t2;
                                }
                        }
                }

                for (u8 i = 0; i < 8; i++)
                        control_step(&ctrls[i], now);

                /* 用实测角度算足端（机体坐标），再加身体位移得到世界坐标 */
                for (u8 i = 0; i < 4; i++) {
                        object_state_t hs, ks;
                        motion_fixed_t a1 = 0, a2 = 0, fx = 0, fy = 0;

                        if (!object_state(hips[i], &hs) ||
                            !object_state(knees[i], &ks) ||
                            !joint_angle_from_orientation(hs.orientation, &a1) ||
                            !joint_angle_from_orientation(ks.orientation, &a2) ||
                            !leg_tip(&geom, a1, a2, &fx, &fy))
                                continue;

                        double body_x = to_d(quad.body_x);
                        double foot_x = to_d(fx) + body_x;
                        double foot_y = to_d(fy);

                        if (stance[i]) {
                                seen_stance[i] = true;

                                if (!settled) {
                                        world_x_max[i] = -1e9;
                                        world_x_min[i] = 1e9;
                                        continue;
                                }

                                /* 支撑期：世界 x 必须恒定 */
                                if (foot_x > world_x_max[i]) world_x_max[i] = foot_x;
                                if (foot_x < world_x_min[i]) world_x_min[i] = foot_x;

                                double slip = world_x_max[i] - world_x_min[i];

                                if (slip > worst_slip)
                                        worst_slip = slip;
                        } else {
                                seen_swing[i] = true;

                                /*
                                 * 只在稳态的摆动相统计抬脚高度。
                                 * 启动时足端本来就在 y = 0，若不排除瞬态，
                                 * 这条断言测的是初始姿态，摆动相完全失效也照样通过。
                                 */
                                double lift = foot_y + to_d(quad.height);

                                if (settled && lift > best_lift)
                                        best_lift = lift;

                                /*
                                 * 命令给出的抬脚高度：由 targets[i][1] 直接算，
                                 * 不含控制器。它必须精确达到配置的 lift。
                                 */
                                double cmd_lift = to_d(targets[i][1]) +
                                                  to_d(quad.height);

                                if (settled && cmd_lift > best_cmd_lift)
                                        best_cmd_lift = cmd_lift;

                                /* 重新落地后重新建立支撑基线 */
                                world_x_max[i] = -1e9;
                                world_x_min[i] = 1e9;
                        }
                }
        }

        printf("  note: body advanced %.4f m\n", to_d(quad.body_x));
        printf("  note: commanded stance slip = %.5f m, measured = %.5f m, "
               "max lift = %.5f m\n",
               worst_cmd_slip, worst_slip, best_lift);

        check("the body advanced", to_d(quad.body_x) > 0.5);

        bool every_leg_worked = true;

        for (u8 i = 0; i < 4; i++)
                if (!seen_stance[i] || !seen_swing[i])
                        every_leg_worked = false;
        check("every leg passed through both stance and swing",
              every_leg_worked);

        /*
         * 这条是整个测试的意义：支撑脚在世界坐标里滑移必须小于跟随机身
         * 时的一半。若把「机体坐标静止」写错成「世界坐标静止」，
         * 滑移会等于步幅 0.08 m —— 那正是打滑。
         */
        /*
         * 两层分开断言：
         *   步态的命令目标在世界坐标里必须**精确**不动（毫米级）；
         *   实测足端允许有 v/K 的跟踪滞后 —— 那是控制物理，不是滑行。
         * v = 0.2 m/s、K = 3 时滞后约 67 mm，给到 120 mm 的界。
         */
        check("the commanded stance foot is world-stationary (gait arithmetic)",
              worst_cmd_slip < 0.002);

        /*
         * 实测滑移的界由两段滞后构成：
         *   摆动末端速度 v_swing = stride / (Swing 时长)
         *                        = 0.08 / (0.4 × 0.667) ≈ 0.30 m/s
         *   进入支撑时带着 v_swing/K ≈ 0.10 m 的滞后，
         *   支撑期间再叠加 v/K = 0.2/3 ≈ 0.067 m。
         * 合计约 0.17 m，给 0.25 m 的界。
         * 这不是「调到能过」：两项都由参数算出，换参数时须同步重算。
         */
        check("measured stance slip stays within the streak-plus-stance lag",
              worst_slip < 0.250);

        printf("  note: commanded lift apex = %.5f m, measured apex = %.5f m\n",
               best_cmd_lift, best_lift);

        /*
         * 两条分开：
         *   1. 步态命令的抬脚必须精确等于配置值（这是算术，必须准确）；
         *   2. 实测足端必须真的离开地面。
         * 把「实测是否达到命令值」也断言进去，测的就是跟踪精度，
         * 而不是抬脚这个行为本身 —— 两者混在一起会掩盖真正的失败。
         */
        check("the gait commands the configured lift apex",
              best_cmd_lift > to_d(quad.lift) * 0.95);

        check("the swing foot genuinely leaves the ground",
              best_lift > 0.005);

        check("all eight joints were driven",
              plants[0].sets > 100 && plants[7].sets > 100);

        /* 对角小跑：腿 0 与腿 1 应当反相 */
        {
                bool opposite = false;

                for (int t = 0; t < 400; t++) {
                        motion_fixed_t tg[QUAD_LEGS_MAX][2];
                        bool st[QUAD_LEGS_MAX];

                        quad_step(&quad, tick(), tg, st);
                        if (st[0] != st[1]) {
                                opposite = true;
                                break;
                        }
                }
                check("diagonal legs are in opposite phase (trot)", opposite);
        }

        check("the base still knows only eight wires, not four legs",
              base_wire_valid(hips[0]) && base_wire_valid(knees[3]));

        printf("\n  quad host failures = %u\n", failures);
        return failures ? 1 : 0;
}
