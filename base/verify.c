/*
 * verify.c — 基座语义与统一模型的验证
 *
 * 这里验证的不是「代码有没有 bug」，而是「这套模型能不能成立」。
 * 第一条尤其重要：如果「大脑也是普通接线」在基座里留了任何特例，
 * 整个「一切皆是运动物件」就不成立了，而这里会直接失败。
 */
#include "base.h"
#include "fixed.h"
#include "object.h"
#include "report.h"
#include "verify.h"

static int failures;

/*
 * 每次自检只在一个确定的提交时刻上提交命令。
 *
 * 早先的写法把提交时刻绑在"最近一次 object_run"上：测试顺序一变，
 * 或者自检少跑了一段，同一个事件就会从新鲜变成过期，失败也
 * 说不清是语义错了还是测试自己漂了。现在时刻是测试自己的局部量，
 * 显式推进，互不影响。
 */
#define MS 1000000ULL

/*
 * 提交时刻从 0 起算。make_event() 造出的事件都带 1 秒新鲜度窗口，
 * 因此在这个基线上的提交必然落在窗口内，不依赖任何前序测试。
 */
static u64 clk;

static void check(const char *what, bool ok)
{
        arch_puts(ok ? "  ok   " : "  FAIL ");
        arch_puts(what);
        arch_puts("\n");
        if (!ok)
                failures++;
}

/* 句柄打印成「槽位/世代」，人读得懂，也能看出换大脑时世代在变 */
static void print_wire(wire_t w)
{
        arch_putdec(WIRE_INDEX(w));
        arch_puts("/");
        arch_putdec(WIRE_GEN(w));
}

static motion_event_t make_event(wire_t wire, wire_t frame, u64 dur,
                                 u8 dof, motion_fixed_t m0, motion_fixed_t m1)
{
        motion_event_t ev;
        ev.wire = wire;
        ev.frame = frame;
        ev.issued_at_ns = 0;
        ev.duration_ns = dur;
        ev.dof = dof;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = 0;
        ev.motion[0] = m0;
        ev.motion[1] = m1;
        return ev;
}

/* 带发出时刻的事件：新鲜度测试与执行时钟推进后的提交都要用它。 */
static motion_event_t make_event_at(wire_t wire, wire_t frame, u64 issued, u64 dur,
                                    u8 dof, motion_fixed_t m0, motion_fixed_t m1)
{
        motion_event_t ev = make_event(wire, frame, dur, dof, m0, m1);
        ev.issued_at_ns = issued;
        return ev;
}

/*
 * 测试侧独立复算量纲换算（设计决定 18），不与 object.c 共用实现。
 *
 * 积分用「余数进位」：每步把精确的 v * period_ns 累加进余数，
 * 只把除得尽的整位写进位置，零头留给下一步。因此 N 步之后的位置
 * 是 floor(N * v * period_ns / 1e9) 的最近值，与步数分配无关 ——
 * 这正是「同一世界速度、不同周期给出同一位置」所需要的模型。
 *
 *   q_dist(v, period_ns, n)  N 步之后相对世界的累计位移（Q32.32 米）
 */
static motion_fixed_t q_dist(motion_fixed_t v, u64 period_ns, u32 n)
{
        motion_fixed_t carry = 0;
        motion_fixed_t pos = 0;

        for (u32 k = 0; k < n; k++) {
                motion_fixed_t step;
                motion_fixed_t next_carry;
                motion_fixed_t next_pos;

                if (fixed_velocity_step_checked(v, period_ns, carry,
                                                &step, &next_carry) != FIXED_OK ||
                    fixed_add_checked(pos, step, &next_pos) != FIXED_OK)
                        return 0;

                pos = next_pos;
                carry = next_carry;
        }
        return pos;
}

/* ------------------------------------------------------------------ */
/* 第一部分：原语语义                                                  */
/* ------------------------------------------------------------------ */

static void verify_primitives(void)
{
        arch_puts("\n--- primitives ---\n");

        wire_t a = object_plug(2, 1000000);
        wire_t b = object_plug(2, 1000000);

        arch_puts("  A = wire ");
        print_wire(a);
        arch_puts(", B = wire ");
        print_wire(b);
        arch_puts("\n\n");

        /* 1. A 相对世界以 100 运动 */
        motion_event_t ev = make_event(a, FRAME_WORLD, 1000000000ULL, 1, 100, 0);
        check("A moves relative to the world",
              base_apply(clk, &ev) == MOTION_OK && object_event_count(a) == 1);
        check("A is not still", !object_is_still(a));

        /*
         * 2. B 相对 A 静止 —— 这就是「静止是一种相对的运动」。
         *    没有单独的「停止」原语：这就是一个 motion 取零的事件。
         */
        ev = make_event(b, a, 1000000000ULL, 1, 0, 0);
        check("B is still relative to A (motion == 0)",
              base_apply(clk, &ev) == MOTION_OK);
        check("B reads as still", object_is_still(b));

        /* 3. 参照系必须是一条有效的接线 */
        ev = make_event(a, 9999, 1000000000ULL, 1, 5, 0);
        check("unknown frame is rejected", base_apply(clk, &ev) == MOTION_E_NO_FRAME);

        /* 4. 目标接线必须存在 */
        ev = make_event(9999, FRAME_WORLD, 1000000000ULL, 1, 5, 0);
        check("unknown wire is rejected", base_apply(clk, &ev) == MOTION_E_NO_WIRE);

        /* 5. dof 越界 */
        ev = make_event(a, FRAME_WORLD, 1000000000ULL, MOTION_DOF_MAX + 1, 1, 0);
        check("dof over the limit is rejected", base_apply(clk, &ev) == MOTION_E_INVAL);

        /* 6. 空事件指针 */
        check("null event is rejected", base_apply(clk, 0) == MOTION_E_INVAL);

        /* 7. 拔出后，接线立即失效，事件被拒 */
        object_unplug(b);
        check("departed wire is no longer valid", !base_wire_valid(b));
        ev = make_event(b, FRAME_WORLD, 1000000000ULL, 1, 7, 0);
        check("event to a departed wire is rejected",
              base_apply(clk, &ev) == MOTION_E_NO_WIRE);

        /* 8. 拔出 B 之后，以 B 为参照系也必须被拒 */
        ev = make_event(a, b, 1000000000ULL, 1, 5, 0);
        check("a departed frame is rejected", base_apply(clk, &ev) == MOTION_E_NO_FRAME);

        object_unplug(a);
}

/* ------------------------------------------------------------------ */
/* 第二部分：统一模型 —— 大脑与手脚都是普通接线                         */
/*                                                                    */
/* 用户的断言：                                                       */
/*   所有组件都看作可运动的；大脑不会动、会思考，也是运动；           */
/*   手脚不会思考、可以动，也是运动。                                 */
/*                                                                    */
/* 下面用同一套原语建一个「大脑」和两个「手脚」，看基座能否区分。      */
/* 结论必须是：区分不了。一旦能区分，模型就塌了。                     */
/* ------------------------------------------------------------------ */

static void verify_unified(void)
{
        arch_puts("\n--- unified model: a brain is an ordinary wire ---\n");

        /* 三条线，三个物件。注意没有任何「种类」参数。 */
        wire_t brain1 = object_plug(2, 1000000);   /* 会思考 */
        wire_t hand   = object_plug(2, 1000000);   /* 不会思考，能动手 */
        wire_t foot   = object_plug(2, 1000000);   /* 不会思考，能走 */

        arch_puts("\n  brain = wire ");
        print_wire(brain1);
        arch_puts(", hand = wire ");
        print_wire(hand);
        arch_puts(", foot = wire ");
        print_wire(foot);
        arch_puts("\n\n");

        /* ---- 断言 1：大脑是一条普通接线 ---- */
        check("brain attaches through the same base_arrive",
              brain1 != WIRE_NONE);
        check("brain occupies no special slot", base_wire_valid(brain1));

        /*
         * 「会思考」就表现为这条线上的一个自由度，只是它的物理含义
         * 叫「认知」。基座不认识这个词，它只看到 motion[0]。
         */
        motion_event_t ev = make_event(brain1, FRAME_WORLD, 1000000000ULL, 1, 1, 0);
        check("base_apply drives the brain like any object",
              base_apply(clk, &ev) == MOTION_OK);
        check("thinking is motion (brain motion[0] == 1)",
              object_motion(brain1, 0) == 1);

        /* ---- 断言 2：手脚也是普通接线 ---- */
        ev = make_event(hand, FRAME_WORLD, 1000000000ULL, 1, 50, 0);
        check("hand moves (motion[0] == 50)",
              base_apply(clk, &ev) == MOTION_OK && object_motion(hand, 0) == 50);

        ev = make_event(foot, FRAME_WORLD, 1000000000ULL, 1, 30, 0);
        check("foot moves (motion[0] == 30)",
              base_apply(clk, &ev) == MOTION_OK && object_motion(foot, 0) == 30);

        /*
         * ---- 断言 3：基座在结构上无法区分二者 ----
         *
         * 这不是一句断言能「跑」出来的，而是由类型的形状决定的：
         * base_arrive()   不带任何参数
         * motion_event_t = { wire, frame, duration_ns, dof, motion[] }
         * 两者都没有「种类」字段，所以基座没有任何依据去区分。
         */
        arch_puts("  note: base_arrive() takes no arguments and motion_event_t carries\n");
        arch_puts("        no 'kind' field,\n");
        arch_puts("        so the base is structurally unable to tell them apart\n");

        /* ---- 断言 4：换大脑 = depart + arrive，手脚不受影响 ---- */
        motion_fixed_t hand_before = object_motion(hand, 0);

        object_unplug(brain1);                     /* 旧大脑离开 */
        wire_t brain2 = object_plug(2, 1000000);   /* 新大脑到来 */

        check("old brain departed cleanly", !base_wire_valid(brain1));
        check("new brain arrives as an ordinary wire", base_wire_valid(brain2));
        check("limbs kept their motion through the brain swap",
              object_motion(hand, 0) == hand_before);

        arch_puts("  new brain reads the limb directly: hand motion[0] = ");
        arch_putdec((u64)object_motion(hand, 0));
        arch_puts("\n  (this is what 'objects are the memory' means)\n");

        /* ---- 断言 5：大脑可以叠大脑，没有特权顶层 ---- */
        ev = make_event(brain2, FRAME_WORLD, 1000000000ULL, 1, 2, 0);
        check("a brain can itself be driven by a higher brain",
              base_apply(clk, &ev) == MOTION_OK && object_motion(brain2, 0) == 2);

        /* 同一条线上，既可以是大脑（下令者），也可以是物件（被驱动者） */
        arch_puts("  note: the same wire is both a brain and a drivable object;\n");
        arch_puts("        'brain' is a role, not a type in the base\n");

        /*
         * 归还本节占用的接线。
         *
         * brain1 已被换下（上面 unplug 过），但 hand、foot 与继任的 brain2
         * 仍然插着。自检不是展示品：如果它留下物件，后面测到的「容量」
         * 就不是系统真实的容量，而是被自检吃掉一部分的容量。
         */
        object_unplug(brain2);
        object_unplug(hand);
        object_unplug(foot);
}

/* ------------------------------------------------------------------ */
/* 第三部分：R8 —— 物件可自述                                         */
/*                                                                    */
/* 需求是「所有物件都要有 3D 定位信息，以及其他运动物件该有的特性」。  */
/* 答案不是加进基座，而是加进物件，并让物件能自述。                    */
/* 这一节验证：物件能申报能力、能持有位姿，而基座里没有任何路径能碰到。*/
/* ------------------------------------------------------------------ */

/* Q32.32：把整数写成定点。用乘法而非左移，负数左移在 C 里是未定义行为 */
#define Q(x) ((motion_fixed_t)(x) * 4294967296LL)

static void verify_self_description(void)
{
        arch_puts("\n--- R8: an object describes and holds its own state ---\n");

        /* 一个三自由度、弧度单位的物件 */
        wire_t part = object_plug(3, 1000000);

        arch_puts("\n  part = wire ");
        print_wire(part);
        arch_puts("\n\n");

        /* 先把结构体填满，避免「可能未初始化」的告警 */
        object_capability_t cap;
        cap.dof = 3;
        cap.mass = Q(3) / 2;                 /* 1.5，Q32.32 */
        for (u8 i = 0; i < MOTION_DOF_MAX; i++) {
                cap.limit_min[i] = 0;
                cap.limit_max[i] = 0;
                cap.max_rate[i]  = 0;
        }
        cap.limit_min[0] = Q(-90);
        cap.limit_max[0] = Q(90);
        cap.max_rate[0]  = Q(2);

        object_capability_t got;
        got.dof = 0;
        got.mass = 0;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++) {
                got.limit_min[i] = 0;
                got.limit_max[i] = 0;
                got.max_rate[i]  = 0;
        }

        check("capability is unavailable before the object describes itself",
              !object_capability(part, &got));

        check("a well-formed description is accepted",
              object_describe(part, &cap));
        check("capability becomes readable after describing",
              object_capability(part, &got) && got.dof == 3);
        check("limits survive the round trip",
              got.limit_min[0] == Q(-90) && got.limit_max[0] == Q(90));
        check("mass survives the round trip", got.mass == Q(3) / 2);

        /* 物件持有自己的位姿：位置 (100, 0, 0)，姿态为单位四元数 */
        object_state_t st;
        for (u8 i = 0; i < 3; i++) {
                st.position[i] = 0;
                st.linear[i]   = 0;
                st.angular[i]  = 0;
        }
        for (u8 i = 0; i < 4; i++)
                st.orientation[i] = 0;
        st.position[0]    = Q(100);
        st.orientation[0] = Q(1);            /* w = 1，无旋转 */

        object_set_state(part, &st);

        object_state_t back;
        for (u8 i = 0; i < 3; i++) {
                back.position[i] = 0;
                back.linear[i]   = 0;
                back.angular[i]  = 0;
        }
        for (u8 i = 0; i < 4; i++)
                back.orientation[i] = 0;

        bool got_state = object_state(part, &back);
        check("pose is readable back",
              got_state && back.position[0] == Q(100));
        check("orientation is a unit quaternion (w == 1)",
              got_state && back.orientation[0] == Q(1));

        arch_puts("  note: none of this is reachable from base.h --\n");
        arch_puts("        the base's whole surface is arrive/depart/apply/valid\n");

        object_unplug(part);
}

/* ------------------------------------------------------------------ */
/* 第四部分：参照系由物件自己解（设计决定 15，方案 A）                  */
/*                                                                    */
/* 基座只校验 frame 这条线当下有效，不解它。解 frame 要读参照系物件的  */
/* 运动 —— 那是世界信息，只能由物件之间直连完成。                      */
/* ------------------------------------------------------------------ */

static void verify_frame_resolution(void)
{
        arch_puts("\n--- R5: objects resolve their own frame (option A) ---\n\n");

        wire_t a = object_plug(2, 1000000);
        wire_t b = object_plug(2, 1000000);
        wire_t c = object_plug(2, 1000000);
        motion_fixed_t r[MOTION_DOF_MAX];

        /* A 相对世界 100；B 相对 A 5；C 相对 B 2 */
        motion_event_t ev = make_event(a, FRAME_WORLD, 1000000000ULL, 1, 100, 0);
        base_apply(clk, &ev);
        ev = make_event(b, a, 1000000000ULL, 1, 5, 0);
        base_apply(clk, &ev);
        ev = make_event(c, b, 1000000000ULL, 1, 2, 0);
        base_apply(clk, &ev);

        check("B resolves to 105 relative to the world",
              object_resolve(b, r) && r[0] == 105);
        check("C resolves through a two-level chain to 107",
              object_resolve(c, r) && r[0] == 107);

        /* 相对静止：B 相对 A 取零，解出来就等于 A 的运动 */
        ev = make_event(b, a, 1000000000ULL, 1, 0, 0);
        base_apply(clk, &ev);
        check("still relative to A resolves to A's own motion (100)",
              object_resolve(b, r) && r[0] == 100);

        /*
         * 成环：基座能接受（每次只看 frame 这条线是否有效），
         * 只有物件侧沿链走才能发现。
         */
        ev = make_event(a, c, 1000000000ULL, 1, 1, 0);
        check("the base accepts a cyclic frame (it cannot see chains)",
              base_apply(clk, &ev) == MOTION_OK);
        check("the object side detects the cycle",
              !object_resolve(c, r));

        /* 参照系离开后，链断开，解算必须失败而不是给出旧值 */
        ev = make_event(a, FRAME_WORLD, 1000000000ULL, 1, 100, 0);
        base_apply(clk, &ev);
        object_unplug(a);
        check("a departed frame breaks the chain",
              !object_resolve(b, r));

        object_unplug(b);
        object_unplug(c);
}

/* ------------------------------------------------------------------ */
/* 第四部分（续）：参照系姿态旋转                                       */
/*                                                                    */
/* 参照系转了角度，相对它的运动也要跟着转，才能得到相对世界的运动。    */
/* ------------------------------------------------------------------ */

static bool near(motion_fixed_t a, motion_fixed_t b)
{
        motion_fixed_t d;

        if (fixed_sub_checked(a, b, &d) != FIXED_OK)
                return false;
        if (d == INT64_MIN)
                return false;
        if (d < 0)
                d = -d;
        return d < Q(1) / 1000;          /* 定点舍入误差容限 0.001 */
}

static void set_orientation(wire_t w, motion_fixed_t qw, motion_fixed_t qx,
                            motion_fixed_t qy, motion_fixed_t qz)
{
        object_state_t st;
        for (u8 i = 0; i < 3; i++) {
                st.position[i] = 0;
                st.linear[i]   = 0;
                st.angular[i]  = 0;
        }
        st.orientation[0] = qw;
        st.orientation[1] = qx;
        st.orientation[2] = qy;
        st.orientation[3] = qz;
        object_set_state(w, &st);
}

static void verify_frame_rotation(void)
{
        arch_puts("\n--- R5: motion rotates with the frame's orientation ---\n\n");

        wire_t a = object_plug(2, 1000000);
        wire_t b = object_plug(2, 1000000);
        motion_fixed_t r[MOTION_DOF_MAX];

        /* A 静止于世界；B 相对 A 沿 A 的 x 轴运动 10 */
        motion_event_t ev = make_event(a, FRAME_WORLD, 1000000000ULL, 3, 0, 0);
        base_apply(clk, &ev);
        ev = make_event(b, a, 1000000000ULL, 3, Q(10), 0);
        base_apply(clk, &ev);

        check("an unrotated frame leaves the motion unchanged (10, 0, 0)",
              object_resolve(b, r) && r[0] == Q(10) && r[1] == 0);

        /* A 绕 z 轴转 180°：q = {0, 0, 0, 1}，结果应精确为 (-10, 0, 0) */
        set_orientation(a, 0, 0, 0, Q(1));
        check("a frame turned 180 deg about z gives (-10, 0, 0)",
              object_resolve(b, r) && r[0] == -Q(10) && r[1] == 0 && r[2] == 0);

        /* A 绕 z 轴转 90°：q = {cos45, 0, 0, sin45}，结果约为 (0, 10, 0) */
        motion_fixed_t c45 = 3037000500LL;   /* 0.70710678 x 2^32 */
        set_orientation(a, c45, 0, 0, c45);
        bool ok = object_resolve(b, r);
        check("a frame turned 90 deg about z gives (0, 10, 0)",
              ok && near(r[0], 0) && near(r[1], Q(10)) && near(r[2], 0));

        /* 相对 A 静止：无论 A 怎么转，解出来都等于 A 自身的运动（这里为 0） */
        ev = make_event(b, a, 1000000000ULL, 3, 0, 0);
        base_apply(clk, &ev);
        check("still relative to a rotated frame stays still",
              object_resolve(b, r) && r[0] == 0 && r[1] == 0 && r[2] == 0);

        object_unplug(a);
        object_unplug(b);
}

/* ------------------------------------------------------------------ */
/* 第五部分：执行模型（设计决定 16）                                    */
/*                                                                    */
/* 基座不调度。各物件按自己申报的 period_ns 被执行层推进，时间由调用者  */
/* 给出。这里用合成时钟，结果完全确定。                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */

static void verify_execution(void)
{
        arch_puts("\n--- execution model: velocity integrated at each object's own period ---\n\n");

        wire_t fast     = object_plug(2, 1 * MS);   /* 每 1 ms 一步 */
        wire_t slow     = object_plug(2, 3 * MS);   /* 每 3 ms 一步 */
        wire_t follower = object_plug(2, 1 * MS);   /* 相对 fast 静止 */
        wire_t idle     = object_plug(0, 0);        /* 未申报周期 */

        motion_event_t ev = make_event(fast, FRAME_WORLD, 1000000000ULL, 1, Q(1), 0);
        base_apply(clk, &ev);                            /* 1 m/s */
        ev = make_event(slow, FRAME_WORLD, 1000000000ULL, 1, Q(1), 0);
        base_apply(clk, &ev);                            /* 同样 1 m/s：量纲对照 */
        ev = make_event(follower, fast, 1000000000ULL, 1, 0, 0);
        base_apply(clk, &ev);

        /*
         * 推 10 ms：执行层第一次看见物件只建立排期，不凭空积分一个
         * 尚未经过的周期。因此 1 ms 周期的物件在第 2..10 ms 上步进
         * 9 次，3 ms 周期的物件在第 4、7、10 ms 上步进 3 次 ——
         * 两者覆盖的世界时间同为 9 ms。
         */
        u32 step_st = 0;

        for (u64 t = 1; t <= 10; t++)
                object_run(t * MS, &step_st);

        check("fast object stepped 9 times in 9 ms",
              object_step_count(fast) == 9);
        check("slow object stepped 3 times in 9 ms",
              object_step_count(slow) == 3);
        check("an object without a declared period is never stepped",
              object_step_count(idle) == 0);

        /*
         * 量纲冻结（决定 18）的落点：同一个 1 m/s，1 ms 周期走 9 步、
         * 3 ms 周期走 3 步，9 ms 世界时间里前进的距离必须相同。
         * 旧语义（每周期位移）做不到这一点。
         */
        object_state_t st;
        bool ok = object_state(fast, &st);
        check("velocity integrates per period (9 steps x v dt)",
              ok && st.position[0] == q_dist(Q(1), 1 * MS, 9));

        ok = object_state(slow, &st);
        check("slow integrates 3 steps x v dt",
              ok && st.position[0] == q_dist(Q(1), 3 * MS, 3));

        ok = object_state(fast, &st);
        motion_fixed_t fast_pos = ok ? st.position[0] : 0;
        ok = object_state(slow, &st) && ok;
        check("same velocity at 1 ms and 3 ms covers the same distance in 9 ms",
              ok && fast_pos == st.position[0]);

        /*
         * 相对 fast 静止的物件，在世界里跟着 fast 一起走 ——
         * 「静止是一种相对的运动」在时间推进下依然成立。
         */
        ok = object_state(follower, &st);
        check("an object still relative to fast moves with it",
              ok && st.position[0] == fast_pos);

        /* 落后很久之后只走一步，不会一次补跑几十步 */
        u32 late_st = 0;

        object_run(100 * MS, &late_st);
        check("a late run steps once instead of catching up",
              object_step_count(fast) == 10);

        object_unplug(fast);
        object_unplug(slow);
        object_unplug(follower);
        object_unplug(idle);
}

/* ------------------------------------------------------------------ */
/* 第六部分：符合性标准 R1–R3（docs/conformance.md）                    */
/* ------------------------------------------------------------------ */

static void verify_conformance(void)
{
        arch_puts("\n--- conformance: R1-R3 ---\n\n");

        wire_t o = object_plug(2, 1 * MS);
        motion_event_t ev;

        /* R1：单一控制面 —— 静止不需要第二种指令，只是 motion 取零的事件 */
        ev = make_event(o, FRAME_WORLD, 1000000000ULL, 1, 5, 0);
        base_apply(clk, &ev);
        ev = make_event(o, FRAME_WORLD, 1000000000ULL, 1, 0, 0);
        check("R1 a zero-motion event makes the object still",
              base_apply(clk, &ev) == MOTION_OK && object_is_still(o));
        ev = make_event(o, FRAME_WORLD, 1000000000ULL, 0, 0, 0);
        check("R1 a zero-dof event is also accepted as still",
              base_apply(clk, &ev) == MOTION_OK && object_is_still(o));

        /*
         * R2：自持 —— 窗口只约束「何时可被接受」；一旦接受，
         * 物件沿该事件持续运动，不需要基座续喂。
         * 执行时钟已被上一段推到 100 ms：先把钟推进一步，再以当前时刻
         * 为发出时刻提交，避免事件在提交前就已过期。
         */
        u32 r2_st = 0;

        object_run(101 * MS, &r2_st);
        ev = make_event_at(o, FRAME_WORLD, 101 * MS, 2 * MS, 1, Q(4), 0);
        check("R2 a fresh event is accepted",
              base_apply(101 * MS, &ev) == MOTION_OK);
        u32 before = object_step_count(o);
        for (u64 t = 102; t <= 106; t++)
                object_run(t * MS, &r2_st);

        object_state_t st;
        bool ok = object_state(o, &st);
        check("R2 the object keeps stepping after the window has passed",
              object_step_count(o) - before == 5);
        check("R2 position keeps integrating past the window (5 steps x v dt)",
              ok && st.position[0] == q_dist(Q(4), 1 * MS, 5));

        /* R3：自述接线属性 —— 到达时申报的 unit / period_ns 可由大脑直读 */
        u32 unit = 0;
        u64 period = 0;
        check("R3 declared unit and period are readable",
              object_declared(o, &unit, &period) && unit == 2 && period == 1 * MS);

        object_unplug(o);
        check("R3 a departed object no longer declares anything",
              !object_declared(o, &unit, &period));
}

/* ------------------------------------------------------------------ */
/* 第七部分：命令新鲜度（设计决定 10 修订）                             */
/*                                                                    */
/* MOTION_E_DEADLINE 在这里第一次成为可达路径：陈旧命令被拒绝，        */
/* 绝不执行 —— 这是大脑热切换与通道拥塞场景下的安全性质。              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 第七部分：checked Q32.32 算术边界                                   */
/* ------------------------------------------------------------------ */

static void verify_fixed_boundaries(void)
{
        motion_fixed_t out = 0;
        motion_fixed_t delta = 0;
        motion_fixed_t carry = 0;

        arch_puts("\n--- checked Q32.32 arithmetic boundaries ---\n\n");

        check("checked add rejects INT64_MAX + 1",
              fixed_add_checked(INT64_MAX, 1, &out) == FIXED_E_OVERFLOW);
        check("checked add rejects INT64_MIN - 1",
              fixed_add_checked(INT64_MIN, -1, &out) == FIXED_E_OVERFLOW);
        check("checked subtract rejects INT64_MIN - 1",
              fixed_sub_checked(INT64_MIN, 1, &out) == FIXED_E_OVERFLOW);
        check("checked Q32.32 multiply rejects an unrepresentable result",
              fixed_mul_checked(INT64_MAX, INT64_MAX, &out) ==
                      FIXED_E_OVERFLOW);

        check("3 m/s over one second is integrated without signed overflow",
              fixed_velocity_step_checked(Q(3), 1000000000ULL, 0,
                                          &delta, &carry) == FIXED_OK &&
              delta == Q(3) && carry == 0);
        check("INT64_MAX velocity over one second remains representable",
              fixed_velocity_step_checked(INT64_MAX, 1000000000ULL, 0,
                                          &delta, &carry) == FIXED_OK &&
              delta == INT64_MAX && carry == 0);
        check("INT64_MIN velocity over one second remains representable",
              fixed_velocity_step_checked(INT64_MIN, 1000000000ULL, 0,
                                          &delta, &carry) == FIXED_OK &&
              delta == INT64_MIN && carry == 0);
        check("an invalid carry boundary is refused",
              fixed_velocity_step_checked(0, 1, 1000000000LL,
                                          &delta, &carry) == FIXED_E_INVAL);
}

static void verify_freshness(void)
{
        /*
         * 提交时刻完全由本测试给出，不读执行时钟、不调用 object_run。
         * 新鲜度是「事件与提交时刻」的关系，与运动环排期无关；把两者
         * 绑在一起会让结论随测试顺序漂移。
         */
        const u64 now = 500 * MS;

        arch_puts("\n--- freshness: a stale command is refused, never executed ---\n\n");

        wire_t o = object_plug(2, 1 * MS);
        motion_event_t ev;

        /* 零长度窗口：deadline == 发出时刻，任何延迟都不可接受 */
        ev = make_event_at(o, FRAME_WORLD, now, 0, 1, Q(1), 0);
        check("a zero-length window is rejected",
              base_apply(now, &ev) == MOTION_E_INVAL);

        /* 未来事件：发出时刻晚于提交时刻，时间来源异常 */
        ev = make_event_at(o, FRAME_WORLD, now + 1, 1000 * MS, 1, Q(1), 0);
        check("an event issued in the future is rejected",
              base_apply(now, &ev) == MOTION_E_INVAL);

        /* 过期：窗口早已闭合，命令绝不下发执行 */
        ev = make_event_at(o, FRAME_WORLD, now - 2 * MS, 1 * MS, 1, Q(1), 0);
        check("an expired event is refused with E_DEADLINE",
              base_apply(now, &ev) == MOTION_E_DEADLINE);

        /*
         * 端点回绕：issued + duration 溢出会伪造一个「永不过期」的窗口。
         * 这里让 issued <= now，排除「未来事件」也能返回 E_INVAL 的可能：
         * 若缺少回绕检查，deadline 截断成 7 之后 now > deadline，
         * 结论会变成 E_DEADLINE，本条即失败。
         */
        ev = make_event_at(o, FRAME_WORLD, UINT64_MAX - 3, 10, 1, Q(1), 0);
        check("a window that wraps the clock counter is rejected",
              base_apply(UINT64_MAX - 2, &ev) == MOTION_E_INVAL);

        /* 边界：提交时刻恰在 deadline 上，仍可接受 */
        ev = make_event_at(o, FRAME_WORLD, now - 2 * MS, 2 * MS, 1, Q(1), 0);
        check("an event exactly at its deadline is still accepted",
              base_apply(now, &ev) == MOTION_OK);

        /* 新鲜窗口内正常接受 */
        ev = make_event_at(o, FRAME_WORLD, now, 1000 * MS, 1, Q(1), 0);
        check("a fresh event inside its window is accepted",
              base_apply(now, &ev) == MOTION_OK);

        object_unplug(o);
}

/* ------------------------------------------------------------------ */
/* 第九部分：执行时钟的安全性质                                        */
/*                                                                    */
/* 时间来自调用者，就可能回退、可能回绕。执行层必须拒绝这两件事，      */
/* 而不是把它们当成「一个很大的增量」默默算下去。                      */
/* ------------------------------------------------------------------ */

static void verify_time_safety(void)
{
        u32 stepped = 0;

        /* 明确高于前序测试用过的节拍，避免本条本身被判定为回退 */
        const u64 base = 500 * MS;

        arch_puts("\n--- execution clock: rollback, overflow, first run ---\n\n");

        check("a forward run advances the execution clock",
              object_run(base, &stepped) == OBJECT_RUN_OK);

        check("a run that goes backwards in time is refused",
              object_run(base - 1, &stepped) ==
                      OBJECT_RUN_E_CLOCK_ROLLBACK);

        check("the same instant is not a rollback",
              object_run(base, &stepped) == OBJECT_RUN_OK);

        /*
         * 首次被执行层看见的物件只建立排期：插上就「已经走了一个周期」
         * 意味着位置里凭空多出一段从未经过的时间。
         */
        wire_t fresh = object_plug(1, 1 * MS);
        motion_event_t ev = make_event(fresh, FRAME_WORLD, 1000000000ULL,
                                       3, Q(1), 0);

        check("a fresh event is accepted before the first run",
              base_apply(base, &ev) == MOTION_OK);

        check("the first run after plugging only schedules",
              object_run(base + 1 * MS, &stepped) == OBJECT_RUN_OK &&
              object_step_count(fresh) == 0);

        check("the next period boundary performs exactly one step",
              object_run(base + 2 * MS, &stepped) == OBJECT_RUN_OK &&
              object_step_count(fresh) == 1);

        check("a null step counter is refused",
              object_run(base + 3 * MS, 0) == OBJECT_RUN_E_INVAL);

        /*
         * 排期加法回绕：now + period_ns 越过 u64 上界时不许截断成一个小数，
         * 否则下一次到期时刻会落在过去，运动环变成每拍都跑。
         */
        check("a scheduling addition that would wrap the clock is refused",
              object_run(UINT64_MAX - 1, &stepped) ==
                      OBJECT_RUN_E_TIME_OVERFLOW);

        check("a refused scheduled time leaves the clock usable",
              object_run(base + 4 * MS, &stepped) == OBJECT_RUN_OK);

        object_unplug(fresh);
}

/* ------------------------------------------------------------------ */
/* 第十部分：dof 尾部与能力申报的纪律                                  */
/*                                                                    */
/* 两条缺陷同源：模型读了「没被声明的东西」。                         */
/*   1. 事件只声明 dof 个分量，实现却照抄全部 6 个 —— 尾部可能是上一条  */
/*      命令的残值或未初始化的栈内容，而且会顺参照链累加进世界速度。    */
/*   2. 能力申报来者不拒 —— dof 越界、区间反向、负速率都会被接受，      */
/*      而申报过的 max_rate 从不被检查。                              */
/* ------------------------------------------------------------------ */

static void cap_zero(object_capability_t *cap)
{
        cap->dof = 0;
        cap->mass = 0;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++) {
                cap->limit_min[i] = 0;
                cap->limit_max[i] = 0;
                cap->max_rate[i]  = 0;
        }
}

static void verify_dof_and_capability(void)
{
        arch_puts("\n--- dof tail and declared capability ---\n\n");

        wire_t hub  = object_plug(2, 1000000);
        wire_t leaf = object_plug(2, 1000000);

        /* hub 只声明 1 个自由度，尾部全部填上哨兵值 */
        motion_event_t ev = make_event(hub, FRAME_WORLD, 1000000000ULL,
                                       1, Q(1), Q(9));
        ev.motion[2] = Q(9);
        ev.motion[3] = Q(9);
        ev.motion[4] = Q(9);
        ev.motion[5] = Q(9);
        check("an event with a polluted tail is still accepted",
              base_apply(clk, &ev) == MOTION_OK);

        check("the tail outside dof is normalised to zero",
              object_motion(hub, 1) == 0 && object_motion(hub, 2) == 0 &&
              object_motion(hub, 3) == 0 && object_motion(hub, 4) == 0 &&
              object_motion(hub, 5) == 0);
        check("the declared component is kept",
              object_motion(hub, 0) == Q(1));

        /*
         * 关键一条：leaf 相对 hub 静止（dof = 0）。hub 的尾部若被照抄，
         * 就会顺着参照链把自己那些「没被命令的角速度」加进 leaf 的
         * 世界运动里 —— 一个 dof=1 的物件因此能推动整条链。
         */
        ev = make_event(leaf, hub, 1000000000ULL, 0, 0, 0);
        check("a still child is accepted",
              base_apply(clk, &ev) == MOTION_OK);

        motion_fixed_t r[MOTION_DOF_MAX];
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                r[i] = 0;
        check("the tail cannot leak through the frame chain",
              object_resolve(leaf, r) && r[0] == Q(1) && r[1] == 0 &&
              r[2] == 0 && r[3] == 0 && r[4] == 0 && r[5] == 0);

        object_unplug(leaf);

        /* ---- 能力申报的校验 ---- */

        object_capability_t cap;
        object_capability_t got;

        cap_zero(&cap);
        cap.dof = MOTION_DOF_MAX + 1;
        check("a description with dof over the limit is refused",
              !object_describe(hub, &cap));
        check("a refused description leaves the object undeclared",
              !object_capability(hub, &got));

        cap_zero(&cap);
        cap.dof = 1;
        cap.limit_min[0] = Q(10);
        cap.limit_max[0] = Q(-10);
        check("a description with an inverted interval is refused",
              !object_describe(hub, &cap));

        cap_zero(&cap);
        cap.dof = 1;
        cap.max_rate[0] = -Q(1);
        check("a description with a negative rate is refused",
              !object_describe(hub, &cap));

        cap_zero(&cap);
        cap.dof = 1;
        cap.mass = -Q(1);
        check("a description with a negative mass is refused",
              !object_describe(hub, &cap));

        check("still undeclared after four refused descriptions",
              !object_capability(hub, &got));

        cap_zero(&cap);
        cap.dof = 1;
        cap.mass = Q(1);
        cap.limit_min[0] = Q(-1);
        cap.limit_max[0] = Q(1);
        cap.max_rate[0]  = Q(2);
        cap.max_rate[3]  = Q(7);            /* 超出 dof：应被清零 */
        check("a well-formed description is accepted",
              object_describe(hub, &cap));

        check("the inactive axes of a description are zeroed",
              object_capability(hub, &got) && got.max_rate[3] == 0 &&
              got.limit_max[3] == 0);

        /* ---- 申报的速率现在真的会拦命令 ---- */

        ev = make_event(hub, FRAME_WORLD, 1000000000ULL, 1, Q(3), 0);
        check("a command above the declared rate is refused with E_LIMIT",
              base_apply(clk, &ev) == MOTION_E_LIMIT);

        ev = make_event(hub, FRAME_WORLD, 1000000000ULL, 1, Q(2), 0);
        check("a command exactly at the declared rate is accepted",
              base_apply(clk, &ev) == MOTION_OK);

        ev = make_event(hub, FRAME_WORLD, 1000000000ULL, 1, -Q(2), 0);
        check("the same rate in the negative direction is accepted",
              base_apply(clk, &ev) == MOTION_OK);

        ev = make_event(hub, FRAME_WORLD, 1000000000ULL, 1, -Q(3), 0);
        check("an over-rate command in the negative direction is refused",
              base_apply(clk, &ev) == MOTION_E_LIMIT);

        /*
         * INT64_MIN 的幅值是 2^63，取负会在 INT64_MIN 上溢出，
         * 因此它必须被单独判定为超限，而不是绕过检查。
         */
        ev = make_event(hub, FRAME_WORLD, 1000000000ULL, 1, INT64_MIN, 0);
        check("INT64_MIN cannot slip past the rate gate",
              base_apply(clk, &ev) == MOTION_E_LIMIT);

        check("a refused command does not disturb the accepted motion",
              object_motion(hub, 0) == -Q(2));

        /* dof 之外的轴本来就没有分量，速率闸门不该对它们生效 */
        ev = make_event(hub, FRAME_WORLD, 1000000000ULL, 1, Q(1), Q(9));
        check("the tail is not rate-checked because it is not commanded",
              base_apply(clk, &ev) == MOTION_OK);

        object_unplug(hub);

        /* 未申报能力的物件不设限：能力闸门只在申报之后生效 */
        wire_t raw = object_plug(2, 1000000);
        ev = make_event(raw, FRAME_WORLD, 1000000000ULL, 6, Q(9), Q(9));
        check("an object that declared nothing is not rate-limited",
              base_apply(clk, &ev) == MOTION_OK);
        object_unplug(raw);
}

int base_verify(void)
{
        failures = 0;

        verify_primitives();
        verify_unified();
        verify_self_description();
        verify_frame_resolution();
        verify_frame_rotation();
        verify_execution();
        verify_conformance();
        verify_fixed_boundaries();
        verify_freshness();
        verify_time_safety();
        verify_dof_and_capability();

        arch_puts("\n  failures = ");
        arch_putdec((u64)failures);
        arch_puts("\n");

        return failures;
}
