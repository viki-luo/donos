/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_handover.c — 控制律热插拔的宿主测试
 *
 * 要证明的四条：
 *   1. 换控制律之后，物件**实测状态不跳变** —— 交接为零。
 *   2. 新控制律的第一个周期只记时刻、不发命令（它还没见过实测）。
 *   3. 换上去之后行为真的变了：速度环让物件持续前进，
 *      位置环把物件拉回目标角，同一个物件、同一个基座。
 *   4. 安装失败时旧控制器**仍然在**，不会因为一次失败而失去控制。
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/quat.c"
#include "../../base/object.c"
#include "../../brain/control.c"
#include "../../brain/handover.c"

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

/* 一条只前进的时钟：执行层拒绝时间回退 */
static u64 clk_ns = 1000000;

static u64 tick(void)
{
        clk_ns += 1000000;
        return clk_ns;
}

/*
 * 受控对象：有两个模式。
 *   线速度模式：实测线速度向命令靠近，位置随之累积。
 *   角速度模式：实测角速度向命令靠近，姿态四元数随之累积。
 * 同一个物件既能被速度环驱动，也能被位置环驱动 —— 这正是
 * 「基座不问设备类型」在控制侧的对应事实。
 */
typedef struct {
        motion_fixed_t linear_cmd, linear_act;
        motion_fixed_t angular_cmd, angular_act;
        motion_fixed_t angle;           /* 累计角度（rad） */
        motion_fixed_t position;
        u32            sets;
} plant_t;

static bool plant_set(void *ctx, const motion_fixed_t linear[3],
                      const motion_fixed_t angular[3])
{
        plant_t *p = (plant_t *)ctx;

        p->linear_cmd = linear[0];
        p->angular_cmd = angular[2];
        p->sets++;
        return true;
}

static bool plant_sample(void *ctx, object_state_t *out)
{
        plant_t *p = (plant_t *)ctx;
        motion_fixed_t d, sh, ch, half;

        /* 线速度一阶跟随 */
        if (fixed_sub_checked(p->linear_cmd, p->linear_act, &d) == FIXED_OK &&
            fixed_mul_checked(d, Q(0.5), &d) == FIXED_OK)
                fixed_add_checked(p->linear_act, d, &p->linear_act);

        /* 角速度一阶跟随，并把角速度积分进角度 */
        if (fixed_sub_checked(p->angular_cmd, p->angular_act, &d) == FIXED_OK &&
            fixed_mul_checked(d, Q(0.5), &d) == FIXED_OK)
                fixed_add_checked(p->angular_act, d, &p->angular_act);

        /* 每拍 1 ms：角度增量 = ω × 1e-3 */
        if (fixed_mul_checked(p->angular_act, Q(0.001), &d) == FIXED_OK)
                fixed_add_checked(p->angle, d, &p->angle);

        /* 位置每拍前进 v × 1e-3 */
        if (fixed_mul_checked(p->linear_act, Q(0.001), &d) == FIXED_OK)
                fixed_add_checked(p->position, d, &p->position);

        for (u8 i = 0; i < 3; i++) {
                out->linear[i] = 0;
                out->angular[i] = 0;
                out->position[i] = 0;
        }
        for (u8 i = 0; i < 4; i++)
                out->orientation[i] = 0;

        out->linear[0] = p->linear_act;
        out->angular[2] = p->angular_act;
        out->position[0] = p->position;

        /* 姿态 = 绕 z 转 angle */
        if (fixed_div_checked(p->angle, (motion_fixed_t)2 << 32, &half) == FIXED_OK &&
            fixed_sin_cos(half, &sh, &ch) == FIXED_OK) {
                out->orientation[0] = ch;
                out->orientation[3] = sh;
        }

        return true;
}

static const motion_device_ops_t plant_ops = {
        .set_velocity = plant_set,
        .sample = plant_sample,
};

int main(void)
{
        printf("\n=== control handover: swap the source, keep the state ===\n");

        base_init();
        object_init();

        wire_t w = object_plug(6, 1000000);
        check("the controlled wire plugs", w != WIRE_NONE);

        plant_t plant = { 0 };
        check("the plant attaches", object_attach_device(w, &plant_ops, &plant));

        control_source_t src = { 0 };

        /* ---- 1. 装速度环，让物件跑起来 ---- */
        check("a speed loop is installed",
              control_install(&src, CONTROL_SPEED, w, 1000000, Q(1), Q(0.5), Q(2)));
        check("the installed kind is reported",
              src.kind == CONTROL_SPEED && src.installs == 1);

        control_step(&src, tick());             /* 首次只记时刻 */
        for (int i = 0; i < 30; i++) {
                u64 now = tick();
                u32 stepped = 0;

                object_run(now, &stepped);
                control_step(&src, now);
        }

        object_state_t before_swap;

        object_state(w, &before_swap);
        check("the speed loop made the object move",
              before_swap.position[0] > 0 &&
              near_ratio(before_swap.linear[0], Q(1), 200));

        /* ---- 2. 热插拔到位置环 ---- */
        motion_fixed_t pos_before = before_swap.position[0];
        motion_fixed_t angle_before = 0;

        joint_angle_from_orientation(before_swap.orientation, &angle_before);

        check("a position loop is installed over the running object",
              control_install(&src, CONTROL_POSITION, w, 1000000, Q(0.5),
                              Q(0.5), Q(2)));
        check("the new kind replaced the old one",
              src.kind == CONTROL_POSITION && src.installs == 2);

        /*
         * 交接为零：换控制器这个动作本身不碰物件。
         * 位置一点都不能跳 —— 跳了说明有人搬了状态。
         */
        {
                object_state_t after_swap;

                object_state(w, &after_swap);
                check("the swap itself does not touch the object",
                      after_swap.position[0] == pos_before);
        }

        /*
         * 新控制器第一拍只记时刻，不发命令：它还没见过实测。
         */
        check("the new controller's first cycle sends nothing",
              !control_step(&src, tick()));

        /*
         * ---- 3. 位置环真的把角度拉向目标 ----
         *
         * 需要给足时间：目标 0.5 rad，位置环输出的角速度约 0.25 rad/s，
         * 从 0 走到 0.5 约需 2 秒，也就是 2000 个 1 ms 拍。
         * 跑 60 拍只能走 0.015 rad —— 那不是控制器的缺陷，
         * 是我把仿真的时长算错了。
         */
        for (int i = 0; i < 4000; i++) {
                u64 now = tick();
                u32 stepped = 0;

                object_run(now, &stepped);
                control_step(&src, now);
        }

        object_state_t after;

        object_state(w, &after);
        {
                motion_fixed_t angle_now = 0;

                joint_angle_from_orientation(after.orientation, &angle_now);
                check("the position loop drove the angle toward its target",
                      near_ratio(angle_now, Q(0.5), 150));
                check("the angle actually changed after the swap",
                      !near_ratio(angle_now, angle_before, 1));
        }

        /* 物件身份与基座接线都没变：同一个物件、同一条线 */
        check("the object is still the same wire and still valid",
              base_wire_valid(w) && object_has_device(w));

        /* ---- 4. 安装失败时旧控制器不能丢 ---- */
        {
                control_kind_t keep = src.kind;

                check("an invalid install is refused",
                      !control_install(&src, CONTROL_SPEED, w, 0, Q(1), Q(1), Q(1)));
                check("the previous controller survived the failed install",
                      src.kind == keep);
        }

        /* ---- 摘掉之后物件不再被命令 ---- */
        control_uninstall(&src);
        check("after uninstalling there is no controller",
              src.kind == CONTROL_NONE && !control_step(&src, tick()));

        object_detach_device(w);
        object_unplug(w);

        printf("\n  handover host failures = %u\n", failures);
        return failures ? 1 : 0;
}
