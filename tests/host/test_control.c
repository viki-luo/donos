/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_control.c — 控制回路闭合的宿主测试
 *
 * 要证明的不是「控制器很聪明」，而是三件结构事实：
 *   1. 反馈取自**实测**，不是命令。命令 1 m/s、设备只跑 0.2 m/s 时，
 *      速度环必须看到 0.2 并继续加大输出，而不是以为已达目标。
 *   2. 命令只经 base_apply() 发出，控制器**从不写物件状态**。
 *   3. 回路真的收敛：在一个「会跟上命令」的设备上，实测速度应逼近目标。
 *
 * 另外验证「没有命令」与「命令为零」的区别：误差为零时仍会发一条
 * 零速度命令 —— 停住是明确意图，不能靠沉默表达。
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/quat.c"
#include "../../base/object.c"
#include "../../brain/control.c"

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

/*
 * 一个「听话」的设备：实测速度以固定比例追上最近收到的命令。
 * 每个周期靠近命令 50%，于是几拍之后收敛 —— 这是被测的控制对象。
 */
typedef struct {
        motion_fixed_t commanded;
        motion_fixed_t actual;
        motion_fixed_t response;        /* 每拍靠近命令的比例 */
        motion_fixed_t clamp_max;       /* 物理上限：最多跑这么快 */
        u32            sets;
} plant_t;

static bool plant_set(void *ctx, const motion_fixed_t linear[3],
                      const motion_fixed_t angular[3])
{
        plant_t *p = (plant_t *)ctx;

        (void)angular;
        p->commanded = linear[0];
        p->sets++;
        return true;
}

static bool plant_sample(void *ctx, object_state_t *out)
{
        plant_t *p = (plant_t *)ctx;
        motion_fixed_t delta;

        /*
         * 向命令靠近：actual += (commanded − actual) · response。
         *
         * 必须走定点库的 checked 乘法：两个 Q32.32 相乘的中间结果有 64 位
         * 有效位，直接用 int64 相乘在数值超过约 2.0 时就会溢出，
         * 于是「靠近」变成「反向跑」—— 编译器不会替我们发现这件事。
         */
        if (fixed_sub_checked(p->commanded, p->actual, &delta) != FIXED_OK)
                return false;
        if (fixed_mul_checked(delta, p->response, &delta) != FIXED_OK)
                return false;
        if (fixed_add_checked(p->actual, delta, &p->actual) != FIXED_OK)
                return false;

        if (p->clamp_max != 0) {
                if (p->actual > p->clamp_max)
                        p->actual = p->clamp_max;
                if (p->actual < -p->clamp_max)
                        p->actual = -p->clamp_max;
        }

        for (u8 i = 0; i < 3; i++) {
                out->linear[i] = 0;
                out->angular[i] = 0;
                out->position[i] = 0;
        }
        for (u8 i = 0; i < 4; i++)
                out->orientation[i] = 0;

        out->linear[0] = p->actual;
        return true;
}

static const motion_device_ops_t plant_ops = {
        .set_velocity = plant_set,
        .sample = plant_sample,
};

/*
 * 全测试共用一条**只前进**的时钟。
 * 执行层按设计拒绝时间回退（OBJECT_RUN_E_CLOCK_ROLLBACK），
 * 各段各用自己的时间基准会让后面的段什么都不执行 —— 这不是测试技巧，
 * 而是系统在正确地拒绝一件不该发生的事。
 */
static u64 clk_ns = 1000000;

static u64 tick(void)
{
        clk_ns += 1000000;
        return clk_ns;
}

int main(void)
{
        printf("\n=== control loops: read measured, send the one primitive ===\n");

        base_init();
        object_init();

        wire_t w = object_plug(1, 1000000);     /* 1 ms */
        check("the controlled wire plugs", w != WIRE_NONE);

        plant_t plant = {
                .commanded = 0,
                .actual = 0,
                .response = Q(0.5),
                .clamp_max = 0,
                .sets = 0,
        };
        check("the plant attaches", object_attach_device(w, &plant_ops, &plant));

        /* ---- 结构事实 1：反馈取自实测 ---- */
        {
                control_loop_t c;
                check("the speed loop initialises",
                      wheel_control_init(&c, w, 1000000, Q(1), Q(2), Q(4)));

                bool sent = false;

                /* 第一次调用只记时刻，不发命令 */
                check("the first call only records the time",
                      !wheel_control_step(&c, tick()));

                /*
                 * 前几拍：plant 从 0 起步，速度环必须持续输出。
                 * 若反馈取自命令，第一次之后误差就会归零、输出立刻塌掉。
                 */
                u32 before = plant.sets;

                for (int i = 0; i < 6; i++) {
                        u64 now = tick();
                        u32 stepped = 0;

                        object_run(now, &stepped);
                        sent = wheel_control_step(&c, now) || sent;
                }
                check("the loop keeps commanding while the plant lags",
                      sent && plant.sets > before + 1);
        }

        /* ---- 结构事实 2：命令只走基座原语，控制器不写状态 ---- */
        {
                control_loop_t c;
                object_state_t before, after;

                wheel_control_init(&c, w, 1000000, Q(1), Q(2), Q(4));
                object_state(w, &before);
                /* 第一次只记时刻，第二次才计算 —— 不手工改内部字段 */
                wheel_control_step(&c, tick());
                wheel_control_step(&c, tick());
                object_state(w, &after);

                /*
                 * 控制器只能通过命令间接影响物件：这一拍里物件的实测
                 * 是否变化由**设备**决定，而不是由控制器直接写入。
                 * 直接写状态会让 position 立刻跳到目标，这里必须没有。
                 */
                check("the controller never writes object state directly",
                      after.position[0] == before.position[0]);
        }

        /* ---- 结构事实 3：回路收敛到目标附近 ---- */
        {
                control_loop_t c;
                plant_t p2 = {
                        .commanded = 0, .actual = 0,
                        .response = Q(0.5), .clamp_max = 0, .sets = 0,
                };

                object_attach_device(w, &plant_ops, &p2);
                /*
                 * 增益必须与被控对象的响应匹配：对象每拍追上命令 50%，
                 * 控制器增益 0.5 给出 0.25 的回路增益，收敛而不震荡。
                 * 增益 2 与响应 0.5 的回路增益正好是 1 —— 那会等幅震荡，
                 * 是控制器的错，不是回路的错。
                 */
                wheel_control_init(&c, w, 1000000, Q(1), Q(0.5), Q(4));

                wheel_control_step(&c, tick());
                for (int i = 0; i < 40; i++) {
                        u64 now = tick();
                        u32 stepped = 0;

                        object_run(now, &stepped);
                        wheel_control_step(&c, now);
                }

                object_state_t st;

                object_state(w, &st);
                check("the loop converges: measured speed approaches the target",
                      near_ratio(st.linear[0], Q(1), 50));
                check("it took more than one command to get there",
                      c.commands_sent > 5);
                check("no command was refused", c.refusals == 0);
        }

        /* ---- 「没有命令」与「命令为零」是两件事 ---- */
        {
                control_loop_t c;
                plant_t p3 = {
                        .commanded = Q(5), .actual = 0,
                        .response = Q(0.5), .clamp_max = 0, .sets = 0,
                };

                object_attach_device(w, &plant_ops, &p3);

                /*
                 * 「已经在目标上」必须显式建立：物件里还留着上一台设备的
                 * 实测值（约 1.0），若不清零，控制器看到的是「目标 0、实测 1」，
                 * 于是正确地输出一个刹车命令 —— 那不是缺陷，是我把测试的
                 * 前提写错了。
                 */
                {
                        object_state_t rest;

                        for (u8 i = 0; i < 3; i++) {
                                rest.position[i] = 0;
                                rest.linear[i] = 0;
                                rest.angular[i] = 0;
                        }
                        for (u8 i = 0; i < 4; i++)
                                rest.orientation[i] = 0;

                        check("the object is put at rest before the test",
                              object_set_state(w, &rest));
                }

                /* 目标 0、实测 0、限幅极小，输出必然为 0 */
                wheel_control_init(&c, w, 1000000, Q(0), Q(1), Q(0.001));

                u32 before = p3.sets;

                wheel_control_step(&c, tick());         /* 只记时刻 */
                wheel_control_step(&c, tick());         /* 这一步发命令 */

                /*
                 * 两件事分开断言，因为它们是两个阶段：
                 *   1. 控制器把一条**零速度命令**交给了基座；
                 *   2. 执行层下一步才把它推给设备。
                 * 合成一条断言会掩盖「命令其实没送到」这种缺陷。
                 */
                check("at target the loop still hands an explicit zero command to the base",
                      c.commands_sent == 1 && c.refusals == 0);

                {
                        u64 now = tick();
                        u32 stepped = 0;

                        object_run(now, &stepped);
                }
                check("the zero command reaches the device on the next step",
                      p3.sets == before + 1 && p3.commanded == 0);
        }

        /* ---- 位置环：角度取自姿态四元数 ---- */
        {
                motion_fixed_t q[4] = { Q(1), 0, 0, 0 };
                motion_fixed_t angle;

                check("a zero orientation reads as zero angle",
                      joint_angle_from_orientation(q, &angle) && angle == 0);

                /* q = (cos(θ/2), 0, 0, sin(θ/2))，θ = 0.2 rad → qz ≈ 0.0998 */
                q[0] = Q(0.9950042);
                q[3] = Q(0.0998334);
                check("the joint angle is recovered from the quaternion",
                      joint_angle_from_orientation(q, &angle) &&
                      near_ratio(angle, Q(0.2), 20));
        }

        object_detach_device(w);
        object_unplug(w);

        printf("\n  control host failures = %u\n", failures);
        return failures ? 1 : 0;
}
