/*
 * brain_verify.c — 大脑热切换的确定性自检（基座之外）
 *
 * 为什么这一段不放进 base/verify.c：基座的自检只该检查基座。
 * 把「大脑怎么换」写进基座的自检文件，等于在基座里给大脑留了一个
 * 概念位置 —— 而这条边界的整个要点就是基座没有那个位置。
 *
 * 时间基准：用 object_now_ns() 起步。基准与自检已经把执行时钟推到
 * 了很远，若这里自己从 0 开始计时，就是一次真实的时间回退，
 * object_run 会正确地拒绝，物件从此不再前进（决定 16）。
 */
#include "brain.h"
#include "arch.h"
#include "object.h"

#define MILLI 1000000ULL

static int bfail;

static void bcheck(const char *what, bool ok)
{
        arch_puts(ok ? "  ok   " : "  FAIL ");
        arch_puts(what);
        arch_puts("\n");
        if (!ok)
                bfail++;
}

/* Q32.32 */
#define Q(v) ((motion_fixed_t)(v) * 4294967296LL)

int brain_verify(void)
{
        bfail = 0;

        arch_puts("\n--- brain hot swap: the source of events changes, "
                  "the base does not notice ---\n\n");

        wire_t target = object_plug(2, MILLI);
        if (target == WIRE_NONE) {
                arch_puts("  FAIL could not plug the target\n");
                return 1;
        }

        u64 t0 = object_now_ns() + MILLI;

        /* ---- 1. 大脑 A 下令，基座照办 ---- */
        brain_t *a = brain_cruise(target, MILLI);
        bcheck("a brain attaches as an ordinary wire",
               a != 0 && a->wire != WIRE_NONE && base_wire_valid(a->wire));

        brain_tick(a, t0);

        motion_fixed_t v[MOTION_DOF_MAX];
        bcheck("the brain's command reaches the target",
               object_motion(target, 0) == Q(1));
        bcheck("the resolved world velocity matches the command",
               object_resolve(target, v) == OBJECT_RESOLVE_OK &&
               v[0] == Q(1));

        /* ---- 2. 让目标真的走一段 ---- */
        u32 stepped = 0;

        for (u32 k = 1; k <= 10; k++)
                object_run(t0 + k * MILLI, &stepped);

        object_state_t st;
        bcheck("the target actually advanced while brain A steered",
               object_state(target, &st) && st.position[0] > 0);

        motion_fixed_t moved = st.position[0];

        /* ---- 3. 换大脑：A 离开，命令必须继续生效 ---- */
        u64 swap_at = t0 + 11 * MILLI;

        brain_detach(a);
        bcheck("the old brain's wire is gone",
               a->wire == WIRE_NONE && !base_wire_valid(a->wire));

        bcheck("the target keeps moving on the last accepted command",
               object_motion(target, 0) == Q(1));
        bcheck("it did not lose the position it had reached",
               object_state(target, &st) && st.position[0] == moved);

        /*
         * 旧大脑的迟到事件：窗口在换之前就关了。
         * 这一条是「换大脑」与「旧大脑还能遥控」的分界 ——
         * 没有新鲜度，旧大脑下过的命令会永远有效。
         */
        motion_event_t late;
        late.wire = target;
        late.frame = FRAME_WORLD;
        late.issued_at_ns = t0;
        late.duration_ns = 1 * MILLI;
        late.dof = 1;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                late.motion[i] = 0;
        late.motion[0] = Q(9);

        bcheck("a late event from the departed brain is refused as expired",
               base_apply(swap_at, &late) == MOTION_E_DEADLINE);
        bcheck("and the refusal did not overwrite the live command",
               object_motion(target, 0) == Q(1));

        /* ---- 4. 大脑 B 接手：只读现场，不带交接 ---- */
        brain_t *b = brain_shuttle(target, MILLI);
        bcheck("the new brain attaches without any handover state", b != 0);

        brain_tick(b, swap_at);

        /*
         * B 的决策依据是它在物件身上读到的位置（此刻为正），
         * 因此它往反方向推。这个方向不是硬编码的，
         * 而是「上一个大脑把物件留在了哪里」的直接结果 ——
         * 恰恰证明它不需要任何交接物。
         */
        arch_puts("  new command (m/s) x 2^32 = ");
        {
                motion_fixed_t c = object_motion(target, 0);
                if (c < 0) {
                        arch_puts("-");
                        arch_putdec((u64)(-(c / Q(1))));
                } else {
                        arch_putdec((u64)(c / Q(1)));
                }
        }
        arch_puts(" (negative means it reversed)\n");

        bcheck("the new brain reversed the motion, decided from the "
               "object's own position", object_motion(target, 0) == -Q(1) / 2);
        bcheck("the new command replaced the old one instead of adding to it",
               object_resolve(target, v) == OBJECT_RESOLVE_OK &&
               v[0] == -Q(1) / 2);

        /* ---- 5. 大脑自己也是普通物件，可被驱动 ---- */
        motion_event_t drive;
        drive.wire = b->wire;
        drive.frame = FRAME_WORLD;
        drive.issued_at_ns = swap_at;
        drive.duration_ns = 10 * MILLI;
        drive.dof = 1;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                drive.motion[i] = 0;
        drive.motion[0] = Q(2);

        bcheck("a brain is itself an ordinary object and can be driven",
               base_apply(swap_at, &drive) == MOTION_OK &&
               object_motion(b->wire, 0) == Q(2));

        /* ---- 收尾：不留物件，否则后面测到的容量不是真实容量 ---- */
        brain_detach(b);
        object_unplug(target);

        arch_puts("\n  brain failures = ");
        arch_putdec((u64)bfail);
        arch_puts("\n");

        return bfail;
}
