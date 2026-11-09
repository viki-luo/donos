/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_wheel.c — 车轮适配器的宿主边界测试
 *
 * 期望值全部手算，不用「跑一遍看看输出」当标准：
 *
 *   r = 0.1 m，gear = 1，counts_per_rev = 1000
 *   v = 1 m/s
 *     ω_wheel = v / r      = 10 rad/s
 *     ω_motor = 10 × 1     = 10 rad/s
 *     counts/s = 10 / 2π × 1000 = 1591.5494...
 *
 *   反向：1591.5494 counts/s → 1 m/s（往返误差在定点精度内）
 *
 * 还要证明三件事：
 *   - 轮径/减速比只在适配器里换算，物件侧的实测仍是 m/s
 *   - 倒车就是负的线速度，没有单独的「倒车指令」
 *   - 参数非法（半径 0、计数 0）时挂接必须被拒绝，而不是除零
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/quat.c"
#include "../../base/object.c"
#include "../../drivers/motion/wheel_adapter.c"

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

/* 百分比容差比较，用于涉及 2π 的换算 */
static int near_ratio(motion_fixed_t got, motion_fixed_t want, int permille)
{
        motion_fixed_t diff = got > want ? got - want : want - got;
        motion_fixed_t limit;

        limit = (want < 0 ? -want : want) / 1000 * permille;
        return diff <= limit;
}

/* ---- 假电机：把收到的 counts/s 记下来，并按固定速率回计数 ---- */

typedef struct {
        motion_fixed_t last_counts_per_s;
        motion_fixed_t encoder_counts;
        motion_fixed_t step_counts;      /* 每次采样前进的计数 */
        bool           encoder_fail;
} fake_motor_t;

static bool motor_set_counts(void *ctx, motion_fixed_t counts_per_s)
{
        fake_motor_t *m = (fake_motor_t *)ctx;

        m->last_counts_per_s = counts_per_s;
        return true;
}

static bool encoder_read(void *ctx, motion_fixed_t *counts)
{
        fake_motor_t *m = (fake_motor_t *)ctx;

        if (m->encoder_fail)
                return false;

        m->encoder_counts += m->step_counts;
        *counts = m->encoder_counts;
        return true;
}

int main(void)
{
        printf("\n=== wheel adapter: units live here, not in the base ===\n");

        wheel_params_t p = {
                .radius = Q(0.1),
                .gear_ratio = Q(1),
                .counts_per_rev = Q(1000),
                .invert = false,
        };

        /* ---- 纯换算：手算值与实现对比 ---- */
        /*
         * 用比例容差而不是精确相等：2π 在 Q32.32 里只能存到 6.28318530703，
         * 与真值差 1.5e-10；这个误差经过「除以 2π 再乘以每转计数」会被
         * 放大到几百个最低位。把容差写成绝对相等，测的就不是换算式，
         * 而是那个常量恰好取到哪一位。
         */
        motion_fixed_t counts;
        check("1 m/s converts to about 1591.55 counts/s",
              wheel_speed_to_counts(&p, Q(1), &counts) &&
              near_ratio(counts, Q(1591.5494), 1));

        motion_fixed_t back;
        check("1591.55 counts/s converts back to about 1 m/s",
              wheel_counts_to_speed(&p, Q(1591.5494), &back) && near_ratio(back, Q(1), 1));

        /* 倒车 = 负速度，没有单独的倒车指令 */
        check("a negative speed is reverse, with no separate command",
              wheel_speed_to_counts(&p, Q(-0.5), &counts) &&
              near_ratio(counts, Q(-795.7747), 1));

        /* 减速比 2：电机转两圈才走一圈，counts/s 翻倍 */
        p.gear_ratio = Q(2);
        check("gear ratio 2 doubles the counts for the same speed",
              wheel_speed_to_counts(&p, Q(1), &counts) &&
              near_ratio(counts, Q(3183.0988), 1));
        p.gear_ratio = Q(1);

        /* 非法参数必须被拒绝，而不是除零 */
        {
                wheel_params_t bad = p;

                bad.radius = 0;
                check("zero radius is refused instead of dividing by zero",
                      !wheel_speed_to_counts(&bad, Q(1), &counts));

                bad = p;
                bad.counts_per_rev = 0;
                check("zero counts-per-rev is refused",
                      !wheel_counts_to_speed(&bad, Q(100), &back));
        }

        /* ---- 接到物件上：实测仍是 m/s ---- */
        base_init();
        object_init();

        wire_t w = object_plug(1, 1000000);      /* 1 ms 周期 */
        check("object plugs", w != WIRE_NONE);

        fake_motor_t motor = {
                .last_counts_per_s = 0,
                .encoder_counts = 0,
                .step_counts = Q(1.59155),      /* 每 1 ms 走 1.59155 计数 */
                .encoder_fail = false,
        };

        wheel_adapter_t wa = {
                .params = p,
                .motor_set_counts = motor_set_counts,
                .encoder_read = encoder_read,
                .ctx = &motor,
                .last_counts = 0,
                .primed = false,
                .period_ns = 0,
        };

        motion_event_t ev = {
                .wire = w,
                .frame = FRAME_WORLD,
                .issued_at_ns = 0,
                .duration_ns = UINT64_MAX,
                .dof = 1,
                .motion = { Q(1), 0, 0, 0, 0, 0 },
        };
        check("a 1 m/s wheel command is accepted",
              base_apply(0, &ev) == MOTION_OK);

        check("the wheel attaches", wheel_attach(w, &wa));
        check("the object reports a device", object_has_device(w));

        /* 首次排期 + 两次执行：第一次采样只建立基准 */
        u64 now = object_now_ns();
        u32 stepped = 0;
        object_run(now + 1000000ULL, &stepped);
        object_run(object_now_ns() + 1000000ULL, &stepped);
        object_run(object_now_ns() + 1000000ULL, &stepped);

        check("the motor received the converted counts/s",
              near_ratio(motor.last_counts_per_s, Q(1591.55), 1));

        object_state_t st;
        check("state is readable", object_state(w, &st));
        check("the object's measured speed is still in m/s, about 1",
              near_ratio(st.linear[0], Q(1), 5));
        check("the object's measured position is in metres",
              st.position[0] > 0);

        /* 编码器读不到时保持上一次实测，不报零 */
        {
                motion_fixed_t before = st.linear[0];

                motor.encoder_fail = true;
                object_run(object_now_ns() + 1000000ULL, &stepped);
                object_state(w, &st);
                check("a dead encoder keeps the last measurement",
                      st.linear[0] == before);
        }

        wheel_detach(w, &wa);
        object_unplug(w);

        printf("\n  wheel host failures = %u\n", failures);
        return failures ? 1 : 0;
}
