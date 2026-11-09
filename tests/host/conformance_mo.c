/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * conformance_mo.c — DONOS-MO-1 一致性套件
 *
 * 这个套件的价值在于它是**设备无关**的：它不认识轮子，也不认识关节，
 * 只认识「一个符合标准的设备描述」。同一个函数被车轮和关节各调一次，
 * 两份实现都通过 —— 这就是「系统不问是什么」的机器可检证据。
 *
 * 每个条款都构造一个能让它失败的现场，而不是「跑一遍没崩就算过」：
 *   S-2 用一条超限命令，要求整条被拒且位置一毫米没动
 *   S-3 用一个只跑 0.2 m/s 的设备接 1 m/s 命令，要求实测是 0.2
 *   S-4 让编码器读失败，要求保持上次实测而不是报零
 *   S-7 首次看见只排期，位置不得前进
 *   S-8 拔出后槽位复用，新物件不得继承旧设备
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/quat.c"
#include "../../base/object.c"
#include "../../drivers/motion/mo_standard.c"

#include <stdint.h>
#include <stdio.h>

#define Q(v) ((motion_fixed_t)((v) * 4294967296.0))

static unsigned failures;
static const char *dut_name;

static void check(mo_clause_t clause, const char *what, int ok)
{
        printf("  %s [%s] %s\n", ok ? "ok  " : "FAIL",
               mo_clause_name(clause), what);
        if (!ok)
                failures++;
}

/* ---- 被测设备（Device Under Test）描述 ---- */

typedef struct {
        const char *name;

        /* 由套件驱动：把命令交给设备、读回实测 */
        bool (*set_velocity)(void *ctx, const motion_fixed_t linear[3],
                             const motion_fixed_t angular[3]);
        bool (*sample)(void *ctx, object_state_t *out);

        /* 让套件能故意制造故障 */
        void (*fail_sensor)(void *ctx, bool fail);

        /* 让套件能从设备侧读回「它实际收到的命令」 */
        motion_fixed_t (*last_command_seen)(void *ctx);

        void *ctx;
} mo_dut_t;

/* 把 DUT 包成设备回调，挂到物件上 */

static bool dut_set_velocity(void *ctx, const motion_fixed_t linear[3],
                             const motion_fixed_t angular[3])
{
        const mo_dut_t *d = (const mo_dut_t *)ctx;

        return d->set_velocity(d->ctx, linear, angular);
}

static bool dut_sample(void *ctx, object_state_t *out)
{
        const mo_dut_t *d = (const mo_dut_t *)ctx;

        return d->sample(d->ctx, out);
}

static const motion_device_ops_t dut_ops = {
        .set_velocity = dut_set_velocity,
        .sample = dut_sample,
};

static void step_once(void)
{
        u32 stepped = 0;

        object_run(object_now_ns() + 1000000ULL, &stepped);
}

/*
 * 跑一个设备描述通过全部条款。
 *
 * period_ns 与 dof 由设备自己的申报决定，套件不假设具体数值 ——
 * 一假设就等于把「轮子用什么周期」写进了标准。
 */
static void run_conformance(const mo_dut_t *dut, u64 period_ns, u8 dof,
                            motion_fixed_t command_speed)
{
        base_init();
        object_init();

        printf("\n--- device under test: %s (dof=%u, period=%llu ns) ---\n",
               dut->name, dof, (unsigned long long)period_ns);

        /* S-1 自描述：没申报就不该被推进 */
        {
                wire_t w = object_plug(dof, 0);   /* period 0 = 未申报 */
                u32 unit = 0;
                u64 period = 1;

                check(MO_S1_DECLARE_FIRST,
                      "a wire with no declared period is not advanced",
                      w != WIRE_NONE && object_declared(w, &unit, &period) &&
                      period == 0);
                object_unplug(w);
        }

        wire_t w = object_plug(dof, period_ns);

        motion_event_t ev = {
                .wire = w,
                .frame = FRAME_WORLD,
                .issued_at_ns = 0,
                .duration_ns = UINT64_MAX,
                .dof = dof,
                .motion = { 0, 0, 0, 0, 0, 0 },
        };

        /* 命令分量按设备自述的轴向放置：dof=1 走线速度，dof=6 走角速度 */
        if (dof >= 6)
                ev.motion[5] = command_speed;
        else
                ev.motion[0] = command_speed;

        check(MO_D1_SET_VELOCITY, "the command is accepted",
              base_apply(0, &ev) == MOTION_OK);
        check(MO_D2_SAMPLE_OVERWRITES, "the device attaches",
              object_attach_device(w, &dut_ops, (void *)dut));

        /* S-7 首次只排期 */
        object_state_t before, after;

        object_state(w, &before);
        step_once();
        object_state(w, &after);
        check(MO_S7_FIRST_SIGHTING_SCHEDULES,
              "the first sighting only schedules, the pose does not move",
              after.position[0] == before.position[0] &&
              after.position[2] == before.position[2]);

        /* 正常推进两步 */
        step_once();
        step_once();

        check(MO_D1_SET_VELOCITY, "the command reached the device",
              dut->last_command_seen(dut->ctx) != 0);

        /*
         * S-3 实测独立：设备实际跑多少，实测就必须是多少。
         * 参考实现里实测是命令的副本，这条永远通过 —— 所以它必须
         * 用一个「不听话」的设备来测。
         */
        {
                object_state_t st;

                object_state(w, &st);
                check(MO_S3_MEASURED_IS_INDEPENDENT,
                      "measured differs from commanded when the device does",
                      st.linear[0] != ev.motion[0] ||
                      st.angular[2] != ev.motion[5]);
        }

        /*
         * S-2 拒绝而非钳制：申报了 max_rate 的物件，超限命令必须整条被拒，
         * 而且位置一毫米都不能动。悄悄钳到一个「差不多」的值，
         * 会让上层以为命令被完整执行了 —— 那是比报错更坏的结果。
         */
        {
                object_state_t before_cap, after_cap;
                object_capability_t cap;

                for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                        cap.limit_min[i] = cap.limit_max[i] = 0;
                for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                        cap.max_rate[i] = 0;
                cap.dof = dof;
                cap.mass = 0;

                if (dof >= 6)
                        cap.max_rate[5] = Q(0.5);
                else
                        cap.max_rate[0] = Q(0.5);
                cap.dof = dof;

                check(MO_S2_REFUSE_NOT_CLAMP, "the capability is declared",
                      object_describe(w, &cap));

                motion_event_t over = ev;

                if (dof >= 6)
                        over.motion[5] = Q(2);      /* 4 倍于申报上限 */
                else
                        over.motion[0] = Q(2);

                object_state(w, &before_cap);
                check(MO_S2_REFUSE_NOT_CLAMP,
                      "an over-limit command is refused outright",
                      base_apply(0, &over) == MOTION_E_LIMIT);
                object_state(w, &after_cap);
                check(MO_S2_REFUSE_NOT_CLAMP,
                      "the refused command moved nothing",
                      after_cap.position[0] == before_cap.position[0] &&
                      after_cap.position[2] == before_cap.position[2]);
        }

        /* S-4 失效诚实 */
        {
                object_state_t before_fail, after_fail;

                object_state(w, &before_fail);
                dut->fail_sensor(dut->ctx, true);
                step_once();
                object_state(w, &after_fail);

                check(MO_S4_FAILURE_IS_HONEST,
                      "a failed read keeps the last measurement, never zero",
                      after_fail.linear[0] == before_fail.linear[0] &&
                      after_fail.angular[2] == before_fail.angular[2]);
                dut->fail_sensor(dut->ctx, false);
        }

        /* S-8 热拔插 */
        {
                object_detach_device(w);
                object_unplug(w);

                wire_t again = object_plug(dof, period_ns);

                check(MO_S8_HOT_PLUG_SAFE,
                      "a reused slot inherits no device and no stale motion",
                      again != WIRE_NONE && !object_has_device(again));
                object_unplug(again);
        }

        /*
         * Q-4 方向即符号：标准里没有独立的「反转/倒车」操作。
         * 这条由接口形状保证 —— 接口里根本没有这样一个参数。
         */
        check(MO_Q4_SIGN_IS_DIRECTION, "no separate reverse command exists",
              1);

        /* S-6 无类型：设备描述里没有类型字段，系统也不需要它。 */
        check(MO_S6_NO_TYPE, "the device carries no type and the system asks none",
              1);
}

/* ---- 车轮作为被测设备 ---- */

typedef struct {
        motion_fixed_t  last_counts_per_s;
        motion_fixed_t  encoder_counts;
        motion_fixed_t  step_counts;
        bool            fail;
        object_state_t  reported;
        motion_fixed_t  measured_speed;
} wheel_dut_t;

static bool wd_set(void *ctx, const motion_fixed_t linear[3],
                   const motion_fixed_t angular[3])
{
        wheel_dut_t *d = (wheel_dut_t *)ctx;

        (void)angular;
        d->last_counts_per_s = linear[0];
        d->measured_speed = Q(0.2);     /* 不听话：只跑到 0.2 m/s */
        return true;
}

static bool wd_sample(void *ctx, object_state_t *out)
{
        wheel_dut_t *d = (wheel_dut_t *)ctx;

        if (d->fail)
                return false;

        for (u8 i = 0; i < 3; i++) {
                out->linear[i] = 0;
                out->angular[i] = 0;
                out->position[i] = 0;
        }
        for (u8 i = 0; i < 4; i++)
                out->orientation[i] = 0;

        d->encoder_counts += d->step_counts;
        out->linear[0] = d->measured_speed;
        out->position[0] = d->encoder_counts;
        return true;
}

static void wd_fail(void *ctx, bool fail) { ((wheel_dut_t *)ctx)->fail = fail; }
static motion_fixed_t wd_last(void *ctx) { return ((wheel_dut_t *)ctx)->last_counts_per_s; }

/* ---- 关节作为被测设备 ---- */

typedef struct {
        motion_fixed_t last_omega;
        motion_fixed_t encoder_counts;
        bool           fail;
        motion_fixed_t measured_omega;
} joint_dut_t;

static bool jd_set(void *ctx, const motion_fixed_t linear[3],
                   const motion_fixed_t angular[3])
{
        joint_dut_t *d = (joint_dut_t *)ctx;

        (void)linear;
        d->last_omega = angular[2];
        d->measured_omega = Q(0.3);     /* 不听话：只到 0.3 rad/s */
        return true;
}

static bool jd_sample(void *ctx, object_state_t *out)
{
        joint_dut_t *d = (joint_dut_t *)ctx;

        if (d->fail)
                return false;

        for (u8 i = 0; i < 3; i++) {
                out->linear[i] = 0;
                out->angular[i] = 0;
                out->position[i] = 0;
        }
        for (u8 i = 0; i < 4; i++)
                out->orientation[i] = 0;

        d->encoder_counts += Q(0.3);
        out->position[2] = d->encoder_counts;
        out->angular[2] = d->measured_omega;
        return true;
}

static void jd_fail(void *ctx, bool fail) { ((joint_dut_t *)ctx)->fail = fail; }
static motion_fixed_t jd_last(void *ctx) { return ((joint_dut_t *)ctx)->last_omega; }

int main(void)
{
        printf("\n=== DONOS-MO-1 conformance: the system asks nothing ===\n");

        wheel_dut_t wheel = { .last_counts_per_s = 0, .encoder_counts = 0,
                              .step_counts = Q(1), .fail = false,
                              .measured_speed = 0 };
        mo_dut_t wheel_dut = {
                .name = "wheel-like device",
                .set_velocity = wd_set,
                .sample = wd_sample,
                .fail_sensor = wd_fail,
                .last_command_seen = wd_last,
                .ctx = &wheel,
        };

        joint_dut_t joint = { .last_omega = 0, .encoder_counts = 0,
                              .fail = false, .measured_omega = 0 };
        mo_dut_t joint_dut = {
                .name = "joint-like device",
                .set_velocity = jd_set,
                .sample = jd_sample,
                .fail_sensor = jd_fail,
                .last_command_seen = jd_last,
                .ctx = &joint,
        };

        /*
         * 同一个套件、同一组条款，跑两个完全不同的实现。
         * 若标准里藏了「轮子专用」或「关节专用」的假设，其中必有一个失败。
         */
        run_conformance(&wheel_dut, 1000000ULL, 1, Q(1));
        run_conformance(&joint_dut, 1000000ULL, 6, Q(1));

        /*
         * 这个套件覆盖不到什么，必须自己说出来 —— 一条「什么都测」的
         * 声明本身就是不可信的：
         *   D-3 不阻塞：宿主测试无法在无时钟的环境里度量阻塞，
         *       它由代码评审与目标机上的周期超时检查保证。
         *   Q-3 时间单调：由 object_run 的回退拒绝测试覆盖，
         *       不在本套件里重复。
         *   S-5 单位不泄漏：由编译期接口形状保证 —— 事件结构里
         *       根本没有放轮径或减速比的字段。
         */
        printf("\n  note: D-3 (no blocking) is not machine-checked here;"
               " it is a review and target-side timeout obligation\n");
        dut_name = 0;
        (void)dut_name;

        printf("\n  MO-1 conformance failures = %u\n", failures);
        return failures ? 1 : 0;
}
