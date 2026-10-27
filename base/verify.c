/*
 * verify.c — 基座语义与统一模型的验证
 *
 * 这里验证的不是「代码有没有 bug」，而是「这套模型能不能成立」。
 * 第一条尤其重要：如果「大脑也是普通接线」在基座里留了任何特例，
 * 整个「一切皆是运动物件」就不成立了，而这里会直接失败。
 */
#include "base.h"
#include "object.h"
#include "report.h"
#include "verify.h"

static int failures;

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
        ev.duration_ns = dur;
        ev.dof = dof;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = 0;
        ev.motion[0] = m0;
        ev.motion[1] = m1;
        return ev;
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
              base_apply(&ev) == MOTION_OK && object_event_count(a) == 1);
        check("A is not still", !object_is_still(a));

        /*
         * 2. B 相对 A 静止 —— 这就是「静止是一种相对的运动」。
         *    没有单独的「停止」原语：这就是一个 motion 取零的事件。
         */
        ev = make_event(b, a, 1000000000ULL, 1, 0, 0);
        check("B is still relative to A (motion == 0)",
              base_apply(&ev) == MOTION_OK);
        check("B reads as still", object_is_still(b));

        /* 3. 参照系必须是一条有效的接线 */
        ev = make_event(a, 9999, 1000000000ULL, 1, 5, 0);
        check("unknown frame is rejected", base_apply(&ev) == MOTION_E_NO_FRAME);

        /* 4. 目标接线必须存在 */
        ev = make_event(9999, FRAME_WORLD, 1000000000ULL, 1, 5, 0);
        check("unknown wire is rejected", base_apply(&ev) == MOTION_E_NO_WIRE);

        /* 5. dof 越界 */
        ev = make_event(a, FRAME_WORLD, 1000000000ULL, MOTION_DOF_MAX + 1, 1, 0);
        check("dof over the limit is rejected", base_apply(&ev) == MOTION_E_INVAL);

        /* 6. 空事件指针 */
        check("null event is rejected", base_apply(0) == MOTION_E_INVAL);

        /* 7. 拔出后，接线立即失效，事件被拒 */
        object_unplug(b);
        check("departed wire is no longer valid", !base_wire_valid(b));
        ev = make_event(b, FRAME_WORLD, 1000000000ULL, 1, 7, 0);
        check("event to a departed wire is rejected",
              base_apply(&ev) == MOTION_E_NO_WIRE);

        /* 8. 拔出 B 之后，以 B 为参照系也必须被拒 */
        ev = make_event(a, b, 1000000000ULL, 1, 5, 0);
        check("a departed frame is rejected", base_apply(&ev) == MOTION_E_NO_FRAME);

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
              base_apply(&ev) == MOTION_OK);
        check("thinking is motion (brain motion[0] == 1)",
              object_motion(brain1, 0) == 1);

        /* ---- 断言 2：手脚也是普通接线 ---- */
        ev = make_event(hand, FRAME_WORLD, 1000000000ULL, 1, 50, 0);
        check("hand moves (motion[0] == 50)",
              base_apply(&ev) == MOTION_OK && object_motion(hand, 0) == 50);

        ev = make_event(foot, FRAME_WORLD, 1000000000ULL, 1, 30, 0);
        check("foot moves (motion[0] == 30)",
              base_apply(&ev) == MOTION_OK && object_motion(foot, 0) == 30);

        /*
         * ---- 断言 3：基座在结构上无法区分二者 ----
         *
         * 这不是一句断言能「跑」出来的，而是由类型的形状决定的：
         * wire_desc_t   = { id, unit, period_ns, online }
         * motion_event_t = { wire, frame, duration_ns, dof, motion[] }
         * 两者都没有「种类」字段，所以基座没有任何依据去区分。
         */
        arch_puts("  note: wire_desc_t and motion_event_t carry no 'kind' field,\n");
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
              base_apply(&ev) == MOTION_OK && object_motion(brain2, 0) == 2);

        /* 同一条线上，既可以是大脑（下令者），也可以是物件（被驱动者） */
        arch_puts("  note: the same wire is both a brain and a drivable object;\n");
        arch_puts("        'brain' is a role, not a type in the base\n");
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

        object_describe(part, &cap);
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
        base_apply(&ev);
        ev = make_event(b, a, 1000000000ULL, 1, 5, 0);
        base_apply(&ev);
        ev = make_event(c, b, 1000000000ULL, 1, 2, 0);
        base_apply(&ev);

        check("B resolves to 105 relative to the world",
              object_resolve(b, r) && r[0] == 105);
        check("C resolves through a two-level chain to 107",
              object_resolve(c, r) && r[0] == 107);

        /* 相对静止：B 相对 A 取零，解出来就等于 A 的运动 */
        ev = make_event(b, a, 1000000000ULL, 1, 0, 0);
        base_apply(&ev);
        check("still relative to A resolves to A's own motion (100)",
              object_resolve(b, r) && r[0] == 100);

        /*
         * 成环：基座能接受（每次只看 frame 这条线是否有效），
         * 只有物件侧沿链走才能发现。
         */
        ev = make_event(a, c, 1000000000ULL, 1, 1, 0);
        check("the base accepts a cyclic frame (it cannot see chains)",
              base_apply(&ev) == MOTION_OK);
        check("the object side detects the cycle",
              !object_resolve(c, r));

        /* 参照系离开后，链断开，解算必须失败而不是给出旧值 */
        ev = make_event(a, FRAME_WORLD, 1000000000ULL, 1, 100, 0);
        base_apply(&ev);
        object_unplug(a);
        check("a departed frame breaks the chain",
              !object_resolve(b, r));

        object_unplug(b);
        object_unplug(c);
}

/* ------------------------------------------------------------------ */
/* 第五部分：执行模型（设计决定 16）                                    */
/*                                                                    */
/* 基座不调度。各物件按自己申报的 period_ns 被执行层推进，时间由调用者  */
/* 给出。这里用合成时钟，结果完全确定。                                */
/* ------------------------------------------------------------------ */

#define MS 1000000ULL

static void verify_execution(void)
{
        arch_puts("\n--- execution model: each object runs at its own period ---\n\n");

        wire_t fast     = object_plug(2, 1 * MS);   /* 每 1 ms 一步 */
        wire_t slow     = object_plug(2, 3 * MS);   /* 每 3 ms 一步 */
        wire_t follower = object_plug(2, 1 * MS);   /* 相对 fast 静止 */
        wire_t idle     = object_plug(0, 0);        /* 未申报周期 */

        motion_event_t ev = make_event(fast, FRAME_WORLD, 1000000000ULL, 1, 10, 0);
        base_apply(&ev);
        ev = make_event(follower, fast, 1000000000ULL, 1, 0, 0);
        base_apply(&ev);

        for (u64 t = 1; t <= 9; t++)
                object_run(t * MS);

        check("fast object stepped 9 times in 9 ms",
              object_step_count(fast) == 9);
        check("slow object stepped 3 times in 9 ms",
              object_step_count(slow) == 3);
        check("an object without a declared period is never stepped",
              object_step_count(idle) == 0);

        object_state_t st;
        bool ok = object_state(fast, &st);
        check("motion is integrated into position (9 x 10 = 90)",
              ok && st.position[0] == 90);

        /*
         * 相对 fast 静止的物件，在世界里跟着 fast 一起走 ——
         * 「静止是一种相对的运动」在时间推进下依然成立。
         */
        ok = object_state(follower, &st);
        check("an object still relative to fast moves with it (90)",
              ok && st.position[0] == 90);

        /* 落后很久之后只走一步，不会一次补跑几十步 */
        object_run(100 * MS);
        check("a late run steps once instead of catching up",
              object_step_count(fast) == 10);

        object_unplug(fast);
        object_unplug(slow);
        object_unplug(follower);
        object_unplug(idle);
}

int base_verify(void)
{
        failures = 0;

        verify_primitives();
        verify_unified();
        verify_self_description();
        verify_frame_resolution();
        verify_execution();

        arch_puts("\n  failures = ");
        arch_putdec((u64)failures);
        arch_puts("\n");

        return failures;
}
