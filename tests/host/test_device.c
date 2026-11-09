/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_device.c — 真实设备挂接的宿主边界测试
 *
 * 这个测试要证明的核心事实只有一条：
 *   **实测值来自设备，而不是命令的副本。**
 *
 * 参考实现里命令速度被直接抄成实测速度，于是监视器的「实测与命令偏差」
 * 永远是零 —— 偏差检测成了摆设。这里用一个「不听话」的假设备：
 * 命令 1 m/s，它只跑 0.2 m/s。若实测仍显示 1 m/s，说明代码还在抄命令。
 *
 * 同时验证：
 *   - 设备模式下执行层不再自己积分位置（位置只为设备反馈负责）
 *   - sample 失败时保持上一次实测，而不是把它当零
 *   - 摘除设备后回到参考实现的自积分行为
 *   - 槽位复用不继承上一任物件的设备
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/quat.c"
#include "../../base/object.c"
#include "../../monitor/monitor.c"

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

/* ---- 假设备：命令归命令，它只跑自己那个速度 ---- */

typedef struct {
        motion_fixed_t actual_linear;   /* 实际能跑到的线速度 */
        motion_fixed_t reported_pos;    /* 编码器报的位置 */
        bool           fail_sample;     /* 让 sample 失败 */
        u32            set_calls;
        u32            sample_calls;
        motion_fixed_t last_cmd_linear;
} fake_device_t;

static bool fake_set_velocity(void *ctx, const motion_fixed_t linear[3],
                              const motion_fixed_t angular[3])
{
        fake_device_t *d = (fake_device_t *)ctx;

        (void)angular;
        d->set_calls++;
        d->last_cmd_linear = linear[0];
        return true;
}

static bool fake_sample(void *ctx, object_state_t *out)
{
        fake_device_t *d = (fake_device_t *)ctx;

        d->sample_calls++;
        if (d->fail_sample)
                return false;

        for (u8 i = 0; i < 3; i++) {
                out->linear[i] = 0;
                out->angular[i] = 0;
                out->position[i] = 0;
        }
        for (u8 i = 0; i < 4; i++)
                out->orientation[i] = 0;

        out->linear[0] = d->actual_linear;      /* 实测：只跑这么点 */
        out->position[0] = d->reported_pos;
        return true;
}

static const motion_device_ops_t fake_ops = {
        .set_velocity = fake_set_velocity,
        .sample = fake_sample,
};

static unsigned drift_seen;

static void drift_sink(monitor_event_t e, wire_t wire)
{
        (void)wire;
        if (e == MON_E_DRIFT)
                drift_seen++;
}

static void run_one_step(void)
{
        u32 stepped = 0;
        u64 now = object_now_ns();

        object_run(now + 1000000ULL, &stepped);
}

int main(void)
{
        printf("\n=== real device: measured comes from the device ===\n");

        base_init();
        object_init();

        wire_t w = object_plug(1, 1000000);
        check("object plugs", w != WIRE_NONE);

        motion_event_t ev = {
                .wire = w,
                .frame = FRAME_WORLD,
                .issued_at_ns = 0,
                .duration_ns = UINT64_MAX,
                .dof = 1,
                .motion = { Q(1), 0, 0, 0, 0, 0 },
        };
        check("a 1 m/s command is accepted", base_apply(0, &ev) == MOTION_OK);

        fake_device_t dev = {
                .actual_linear = Q(0.2),        /* 只跑 0.2 m/s */
                .reported_pos = Q(0.5),         /* 编码器说它在 0.5 m */
                .fail_sample = false,
                .set_calls = 0,
                .sample_calls = 0,
                .last_cmd_linear = 0,
        };

        check("the device attaches", object_attach_device(w, &fake_ops, &dev));
        check("the object reports it has a device", object_has_device(w));

        run_one_step();         /* 首次只排期 */
        run_one_step();         /* 这一步真正执行 */

        check("the command reached the device at 1 m/s",
              dev.set_calls >= 1 && dev.last_cmd_linear == Q(1));
        check("the device was sampled", dev.sample_calls >= 1);

        object_state_t st;
        check("state is readable", object_state(w, &st));

        /*
         * 这两条是本测试的全部意义：
         * 命令还是 1 m/s，但实测必须是设备报的 0.2 m/s。
         */
        check("the command is still 1 m/s",
              object_motion(w, 0) == Q(1));
        check("the measured speed is the device's 0.2 m/s, not the command",
              st.linear[0] == Q(0.2));
        check("the measured position is the encoder's, not an integral",
              st.position[0] == Q(0.5));

        /* 监视器必须看见这个偏差 —— 参考实现里它永远是零。 */
        drift_seen = 0;
        monitor_init(drift_sink);
        monitor_set_drift_limit(Q(0.05));
        monitor_watch(w);
        monitor_poll(OBJECT_RUN_OK);

        check("the monitor reports the measured-versus-commanded drift",
              drift_seen >= 1);

        /* sample 失败时保持上一次实测，不能把它当零。 */
        dev.fail_sample = true;
        run_one_step();
        check("a failed sample keeps the last measured value",
              object_state(w, &st) && st.linear[0] == Q(0.2));

        /* 摘除设备后回到参考实现：实测重新跟随命令。 */
        object_detach_device(w);
        check("the device detaches", !object_has_device(w));
        run_one_step();
        check("without a device the reference implementation integrates again",
              object_state(w, &st) && st.linear[0] == Q(1));

        object_unplug(w);

        /*
         * 槽位复用不得继承上一任物件的设备：新物件必须是「没有设备」的
         * 干净状态，否则它会对着上一任的电机下命令。
         */
        wire_t reused = object_plug(1, 1000000);
        check("a reused slot does not inherit the previous device",
              reused != WIRE_NONE && !object_has_device(reused));
        object_unplug(reused);

        printf("\n  device host failures = %u\n", failures);
        return failures ? 1 : 0;
}
