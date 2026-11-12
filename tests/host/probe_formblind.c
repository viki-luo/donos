/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * probe_formblind.c — 形态无关控制探针
 *
 * 定位（必须先说清，否则会被误读）：
 *
 *   这个文件证明的是**基座 + 标准的下界**：不需要任何关于「它是什么」的
 *   知识，也能把一个合规物件驱动到目标。它**不是**大脑的写法示范。
 *
 *   本体论第 3 条说「其他信息都是物件直接与上层大脑联络」—— 大脑本来就
 *   **应该**知道形态与用途。把本探针当成大脑范式，就会推出「大脑不该知道
 *   形态」，进而取消物件与大脑之间的那条通道。基座必须无知，大脑必须知道；
 *   本探针只证明前半句。
 *
 * 因此它住在 tests/ 而不是 brain/：它是一致性客户端，不是产品大脑。
 *
 * 它只使用两样东西：
 *   1. 接口约定里的**轴索引**（0..2 线速度 m/s，3..5 角速度 rad/s）——
 *      这是位置约定，不是设备类型；
 *   2. 实测反馈。
 *
 * 它**刻意不读** `unit`：那是物件自述里最接近「身份」的字段。
 * 如果探针读了它，这个实验就失去意义。
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/quat.c"
#include "../../base/object.c"

#include <stdint.h>
#include <stdio.h>

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

#define Q(v) ((motion_fixed_t)((v) * 4294967296.0))

/* ---- 探针本身：只有轴索引与反馈，没有类型 ---- */

typedef struct {
        wire_t         wire;
        u8             axis;        /* 0..5，由调用者给出（能力，不是身份） */
        motion_fixed_t target;
        motion_fixed_t gain;
        motion_fixed_t limit;
        u64            last_ns;
        u32            sent;
} probe_t;

static bool probe_init(probe_t *p, wire_t w, u8 axis, motion_fixed_t target,
                       motion_fixed_t gain, motion_fixed_t limit)
{
        if (p == 0 || axis >= 6 || gain <= 0 || limit <= 0)
                return false;
        if (!base_wire_valid(w))
                return false;

        p->wire = w;
        p->axis = axis;
        p->target = target;
        p->gain = gain;
        p->limit = limit;
        p->last_ns = 0;
        p->sent = 0;
        return true;
}

/*
 * 读数：轴 0..2 读线速度，轴 3..5 读角速度。
 * 这个映射来自标准的位置约定，不来自设备类型 —— 探针不知道、
 * 也不需要知道那个轴背后是电机还是轮子。
 */
static bool read_axis(const object_state_t *st, u8 axis, motion_fixed_t *out)
{
        if (axis >= 6)
                return false;

        if (axis < 3)
                *out = st->linear[axis];
        else
                *out = st->angular[axis - 3];

        return true;
}

static bool probe_step(probe_t *p, u64 now_ns)
{
        object_state_t st;
        motion_fixed_t measured, error, command;
        motion_event_t ev;

        if (p == 0)
                return false;

        /* 第一拍只记时刻：还没见过实测，任何误差都是凭空的 */
        if (p->last_ns == 0) {
                p->last_ns = now_ns;
                return false;
        }
        if (now_ns < p->last_ns || now_ns - p->last_ns < 1000000ULL)
                return false;
        p->last_ns = now_ns;

        if (!object_state(p->wire, &st))
                return false;
        if (!read_axis(&st, p->axis, &measured))
                return false;

        if (fixed_sub_checked(p->target, measured, &error) != FIXED_OK)
                return false;
        if (fixed_mul_checked(error, p->gain, &command) != FIXED_OK)
                return false;
        if (fixed_add_checked(p->target, command, &command) != FIXED_OK)
                return false;

        if (command > p->limit)
                command = p->limit;
        if (command < -p->limit)
                command = -p->limit;

        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = 0;

        ev.wire = p->wire;
        ev.frame = FRAME_WORLD;
        ev.issued_at_ns = now_ns;
        ev.duration_ns = 2000000ULL;    /* 两拍窗口 */
        ev.dof = (u8)(p->axis + 1);     /* 只需覆盖到该轴 */
        ev.motion[p->axis] = command;

        p->sent++;
        return base_apply(now_ns, &ev) == MOTION_OK;
}

/* ---- 两个完全不同的受控对象 ---- */

typedef struct {
        motion_fixed_t cmd_linear, act_linear;
        motion_fixed_t cmd_angular, act_angular;
} plant_t;

static bool plant_set(void *ctx, const motion_fixed_t linear[3],
                      const motion_fixed_t angular[3])
{
        plant_t *p = (plant_t *)ctx;

        p->cmd_linear = linear[0];
        p->cmd_angular = angular[2];
        return true;
}

static bool plant_sample(void *ctx, object_state_t *out)
{
        plant_t *p = (plant_t *)ctx;
        motion_fixed_t d;

        if (fixed_sub_checked(p->cmd_linear, p->act_linear, &d) == FIXED_OK &&
            fixed_mul_checked(d, Q(0.5), &d) == FIXED_OK)
                fixed_add_checked(p->act_linear, d, &p->act_linear);

        if (fixed_sub_checked(p->cmd_angular, p->act_angular, &d) == FIXED_OK &&
            fixed_mul_checked(d, Q(0.5), &d) == FIXED_OK)
                fixed_add_checked(p->act_angular, d, &p->act_angular);

        for (u8 i = 0; i < 3; i++) {
                out->linear[i] = 0;
                out->angular[i] = 0;
                out->position[i] = 0;
        }
        for (u8 i = 0; i < 4; i++)
                out->orientation[i] = 0;

        out->linear[0] = p->act_linear;
        out->angular[2] = p->act_angular;
        return true;
}

static const motion_device_ops_t plant_ops = { plant_set, plant_sample };

static u64 clk_ns = 1000000;

static u64 tick(void)
{
        clk_ns += 1000000;
        return clk_ns;
}

/*
 * 跑一个「设备形态」，参数完全一样：同样的增益、同样的限幅、同样的步进数。
 * 唯一变化的是轴索引与目标值。
 */
static double run_one(u8 axis, motion_fixed_t target, u32 unit_tag)
{
        base_init();
        object_init();

        /*
         * 第一个参数是物件自述的 unit —— 最接近「身份」的字段。
         * 两次运行故意给完全不同的值：探针若依赖它，收敛就会受影响。
         * 探针从不读它（也不调用 object_declared）。
         */
        wire_t w = object_plug(unit_tag, 1000000);
        if (w == WIRE_NONE)
                return -1;

        check("the object is plugged with an arbitrary unit tag",
              w != WIRE_NONE);

        plant_t plant = { 0 };
        object_attach_device(w, &plant_ops, &plant);

        probe_t probe;
        if (!probe_init(&probe, w, axis, target, Q(2), Q(4)))
                return -1;

        probe_step(&probe, tick());
        for (int i = 0; i < 4000; i++) {
                u64 now = tick();
                u32 stepped = 0;

                object_run(now, &stepped);
                probe_step(&probe, now);
        }

        object_state_t st;
        motion_fixed_t measured = 0;

        if (!object_state(w, &st) || !read_axis(&st, axis, &measured))
                return -1;

        object_detach_device(w);
        object_unplug(w);
        return to_d(measured);
}

int main(void)
{
        printf("\n=== form-blind probe: same code, two unrelated devices ===\n");

        /*
         * 设备 A：dof=1，线性轴 0，目标 1.0 m/s
         * 设备 B：dof=6，角速度轴 5，目标 0.5 rad/s
         *
         * 同一份探针代码、同一组增益与限幅、同样的步进数。
         * 探针不知道哪个是「轮子」、哪个是「关节」。
         */
        double a = run_one(0, Q(1.0), 0xA1B2C3D4u);
        double b = run_one(5, Q(0.5), 0x5E6F7A8Bu);

        printf("  note: linear axis reached %.6f (target 1.000000)\n", a);
        printf("  note: angular axis reached %.6f (target 0.500000)\n", b);

        check("the linear device reaches its target", a > 0.95 && a < 1.05);
        check("the angular device reaches its target", b > 0.475 && b < 0.525);

        /*
         * 两条一起看才是本探针的意义：**同一份代码**在两种互不相干的
         * 形态上都收敛。若探针依赖形态，两者不可能同时成立。
         */
        check("one probe satisfies both forms, with no type knowledge",
              a > 0.95 && b > 0.475);

        printf("\n  form-blind probe failures = %u\n", failures);
        return failures ? 1 : 0;
}
