/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_joint.c — 关节适配器与多关节串联的宿主测试
 *
 * 手算基准（counts_per_rad = 1000，zero = 0，gear 无）：
 *   ω = 1 rad/s  → 1000 counts/s
 *   ω = -0.5     → -500 counts/s（反转即负命令）
 *   counts = 1500 → 相对零位 1.5 rad
 *   counts = 500, zero = 500 → 0 rad
 *
 * 还验证机械臂的最小结构：**关节是串联的接线，不是一条超宽命令**。
 * 第 2 关节的 frame 指第 1 关节，第 1 关节的 frame 指世界；
 * 解算后第 2 关节的世界角速度是两者之和 —— 链条自己成立，基座不知情。
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/quat.c"
#include "../../base/object.c"
#include "../../drivers/motion/joint_adapter.c"

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

typedef struct {
        motion_fixed_t last_counts_per_s;
        motion_fixed_t encoder_counts;
        motion_fixed_t step_counts;
        bool           encoder_fail;
} fake_joint_motor_t;

static bool motor_set_counts(void *ctx, motion_fixed_t counts_per_s)
{
        fake_joint_motor_t *m = (fake_joint_motor_t *)ctx;

        m->last_counts_per_s = counts_per_s;
        return true;
}

static bool encoder_read(void *ctx, motion_fixed_t *counts)
{
        fake_joint_motor_t *m = (fake_joint_motor_t *)ctx;

        if (m->encoder_fail)
                return false;

        m->encoder_counts += m->step_counts;
        *counts = m->encoder_counts;
        return true;
}

int main(void)
{
        printf("\n=== joint adapter: one wire is one joint ===\n");

        joint_params_t p = {
                .counts_per_rad = Q(1000),
                .zero_counts = 0,
                .invert = false,
        };

        /* ---- 纯换算 ---- */
        motion_fixed_t counts, angle;

        check("1 rad/s becomes 1000 counts/s",
              joint_speed_to_counts(&p, Q(1), &counts) && counts == Q(1000));
        check("a negative rate is reverse, with no separate command",
              joint_speed_to_counts(&p, Q(-0.5), &counts) && counts == Q(-500));
        check("1500 counts reads as 1.5 rad from zero",
              joint_counts_to_angle(&p, Q(1500), &angle) && angle == Q(1.5));

        p.zero_counts = Q(500);
        check("the zero offset is subtracted before converting",
              joint_counts_to_angle(&p, Q(500), &angle) && angle == 0);
        p.zero_counts = 0;

        {
                joint_params_t bad = p;

                bad.counts_per_rad = 0;
                check("zero counts-per-radian is refused",
                      !joint_speed_to_counts(&bad, Q(1), &counts) &&
                      !joint_counts_to_angle(&bad, Q(1), &angle));
        }

        /* ---- 单关节接到物件上 ---- */
        base_init();
        object_init();

        wire_t shoulder = object_plug(1, 1000000);
        check("the shoulder joint plugs", shoulder != WIRE_NONE);

        motion_event_t ev = {
                .wire = shoulder,
                .frame = FRAME_WORLD,
                .issued_at_ns = 0,
                .duration_ns = UINT64_MAX,
                .dof = 6,
                .motion = { 0, 0, 0, 0, 0, Q(1) },      /* 绕 z 1 rad/s */
        };
        check("a 1 rad/s joint command is accepted",
              base_apply(0, &ev) == MOTION_OK);

        fake_joint_motor_t motor = {
                .last_counts_per_s = 0,
                .encoder_counts = 0,
                .step_counts = Q(1),            /* 每 1 ms 走 1 计数 = 1 mrad */
                .encoder_fail = false,
        };
        joint_adapter_t ja = {
                .params = p,
                .motor_set_counts = motor_set_counts,
                .encoder_read = encoder_read,
                .ctx = &motor,
                .last_counts = 0,
                .primed = false,
                .period_ns = 0,
        };

        check("the joint attaches", joint_attach(shoulder, &ja));

        u32 stepped = 0;
        object_run(object_now_ns() + 1000000ULL, &stepped);
        object_run(object_now_ns() + 1000000ULL, &stepped);
        object_run(object_now_ns() + 1000000ULL, &stepped);

        check("the motor received counts for 1 rad/s",
              near_ratio(motor.last_counts_per_s, Q(1000), 1));

        object_state_t st;
        check("state is readable", object_state(shoulder, &st));
        /*
         * 关节角在**姿态四元数**里，不在 position[] 里：
         * position 按 Q-2 是米，弧度进去就是单位违规。
         * 0.002 rad 的半角是 0.001 rad，故 qw≈1、qz≈0.001。
         */
        {
                double qw = (double)st.orientation[0] / 4294967296.0;
                double qz = (double)st.orientation[3] / 4294967296.0;

                check("the joint angle is expressed as an orientation quaternion",
                      qw > 0.999999 && qw <= 1.000001 &&
                      qz > 0.00099 && qz < 0.00101);
                check("no angle leaks into position, which is metres",
                      st.position[2] == 0);
        }
        check("the measured rate is about 1 rad/s",
              near_ratio(st.angular[2], Q(1), 50));

        /* 编码器失效时保持上一次实测 */
        {
                motion_fixed_t before = st.angular[2];

                motor.encoder_fail = true;
                object_run(object_now_ns() + 1000000ULL, &stepped);
                object_state(shoulder, &st);
                check("a dead encoder keeps the last measurement",
                      st.angular[2] == before);
        }

        /* ---- 两关节串联：参照链自己成立，基座不知情 ---- */
        wire_t elbow = object_plug(1, 1000000);
        check("the elbow joint plugs", elbow != WIRE_NONE);

        /*
         * 肘相对肩，肩相对世界。两条独立的接线，没有任何「手臂」对象。
         * 这正是「基座只认运动物件」的含义：它不知道这两条线是同一条手臂。
         *
         * 参照系由运动事件的 frame 字段确定 —— 没有单独的 set_frame，
         * 因为「相对谁动」本身就是命令的一部分。
         */
        motion_event_t elbow_ev = {
                .wire = elbow,
                .frame = shoulder,
                .issued_at_ns = 0,
                .duration_ns = UINT64_MAX,
                .dof = 6,
                .motion = { 0, 0, 0, 0, 0, Q(0.5) },
        };
        check("a 0.5 rad/s elbow command is accepted",
              base_apply(0, &elbow_ev) == MOTION_OK);

        fake_joint_motor_t elbow_motor = {
                .last_counts_per_s = 0,
                .encoder_counts = 0,
                .step_counts = Q(0.5),
                .encoder_fail = false,
        };
        joint_adapter_t eb = {
                .params = p,
                .motor_set_counts = motor_set_counts,
                .encoder_read = encoder_read,
                .ctx = &elbow_motor,
                .last_counts = 0,
                .primed = false,
                .period_ns = 0,
        };
        check("the elbow attaches", joint_attach(elbow, &eb));

        object_run(object_now_ns() + 1000000ULL, &stepped);
        object_run(object_now_ns() + 1000000ULL, &stepped);

        /* 世界视角的角速度应当是两条命令叠加 */
        motion_fixed_t world[MOTION_DOF_MAX];
        check("the chain resolves",
              object_resolve(elbow, world) == OBJECT_RESOLVE_OK);
        check("the elbow's world rate is the sum along the chain",
              near_ratio(world[5], Q(1.5), 60));

        joint_detach(elbow, &eb);
        object_unplug(elbow);
        joint_detach(shoulder, &ja);
        object_unplug(shoulder);

        printf("\n  joint host failures = %u\n", failures);
        return failures ? 1 : 0;
}
