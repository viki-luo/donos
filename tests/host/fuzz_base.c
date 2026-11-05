/*
 * fuzz_base.c — 基座/物件/定点层的确定性模糊驱动器（宿主侧）
 *
 * 为什么是「确定性模糊」而不是真随机：一次崩溃要能可靠复现，就必须
 * 能重放。用固定种子的 xorshift 和纯函数式随机操作序列，同样的种子
 * 每次都产生同样的字节序列 —— 崩溃报告回来时，把种子贴回去就能复现。
 *
 * 它不进目标镜像，只在本机构建下跑，但跑的就是目标的那份代码：
 *   base.c  +  object.c  +  fixed.c  原件，零桩、零改写。
 *
 * 每一轮随机施加一个操作，并立刻核对不变量。监控的不是「测出多少 bug」，
 * 而是两类东西：
 *   1. 内存/算术安全 —— 用 UBSan（和可用的 ASan）编译，任何未定义行为
 *      直接陷阱；
 *   2. 状态一致性 —— 影子表独立跟踪「哪些接线应当活着」，与
 *      base_wire_valid / object_declared 逐条对账；接线可见即就绪。
 *
 * 断言全部在「合法输入正确、非法输入被安全拒绝」两侧同时做：
 *   调坏的事必须拒绝得干净，而不是边拒绝边污染状态。
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/object.c"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define Q(v) ((motion_fixed_t)(v) * 4294967296LL)

static unsigned long long failures;
static unsigned long long rng_state = 0x9E3779B97F4A7C15ull;

static unsigned long long rnd(void)
{
        unsigned long long x = rng_state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return rng_state = x;
}

/* 均匀取 [0, lim) */
static u64 pick(u64 lim)
{
        return lim == 0 ? 0 : rnd() % lim;
}

static void fail(const char *what)
{
        fprintf(stderr, "FUZZ FAIL: %s (seed=%llu)\n", what, rng_state);
        failures++;
}

/*
 * 不变量核对。每当操作改变了接线集合，就调用一次：
 *   - live 表里每一个都必须是活的（base 与 object 两侧一致）
 *   - base_apply 对所有被接受的事件都得落到 object 层
 *   - 随机伪造的句柄必须被干净地拒绝
 */
static wire_t pop_random_live_or_forge(void);  /* 定义在下方，读取不动 live */

#define LIVE_MAX 64

static wire_t live[LIVE_MAX];
static u32    live_count;

static void oracle(void)
{
        motion_status_t s;
        u32 unit;
        u64 period;

        for (u32 i = 0; i < live_count; i++) {
                if (!base_wire_valid(live[i])) {
                        fail("a live wire is not valid in the base");
                        return;
                }
                if (!object_declared(live[i], &unit, &period)) {
                        fail("a live wire is not declared on the object side");
                        return;
                }
                if (period > OBJECT_PERIOD_NS_MAX) {
                        fail("object_declared returned an out-of-range period");
                        return;
                }

                s = base_apply(0, &(motion_event_t){
                        .wire = live[i],
                        .frame = FRAME_WORLD,
                        .issued_at_ns = 0,
                        .duration_ns = 1,
                        .dof = 0,
                        .motion = { 0 },
                });
                if (s != MOTION_OK) {
                        fail("a freshly accepted zero-motion event to a live "
                             "wire was refused");
                        return;
                }
        }

        /* 伪造句柄的拒绝必须是干净的，不能碰任何活物件 */
        for (u32 i = 0; i < LIVE_MAX; i++) {
                wire_t fake = WIRE_MAKE((u32)pick(1 << 24),
                                        (u32)(1 + pick(64)));
                bool collides = false;

                for (u32 k = 0; k < live_count; k++)
                        if (fake == live[k])
                                collides = true;
                if (collides)
                        fake += (wire_t)1 << 16;

                s = base_apply(0, &(motion_event_t){
                        .wire = fake,
                        .frame = FRAME_WORLD,
                        .issued_at_ns = 0,
                        .duration_ns = 1,
                        .dof = 0,
                        .motion = { 0 },
                });
                if (s != MOTION_E_NO_WIRE) {
                        fail("a forged wire was not refused as NO_WIRE");
                        return;
                }
                (void)i;
        }
}

static wire_t pop_random_live(void)
{
        if (live_count == 0)
                return WIRE_NONE;
        u32 i = (u32)pick(live_count);
        wire_t w = live[i];
        live[i] = live[--live_count];
        return w;
}

static void op_plug(void)
{
        if (live_count == LIVE_MAX)
                return;
        u32 unit = (u32)pick(1000);
        u64 period = pick(3);           /* 0（非法）, 1, 1e9 边界 */
        period = period == 0 ? (pick(2) ? 0 : 1000000000ull)
                             : (1 + pick(1000000));
        wire_t w = object_plug(unit, period);
        if (w == WIRE_NONE)
                return;                 /* 表满或周期非法：干净失败即可 */
        if (WIRE_INDEX(w) == 0 || WIRE_INDEX(w) > 64)
                return fail("object_plug returned an out-of-range slot");
        if (live_count >= LIVE_MAX)
                return;

        /* 发布顺序：接线可见的瞬间，状态必须已经就绪 */
        u32 u2 = 0;
        u64 p2 = 0;
        if (!object_declared(w, &u2, &p2) || u2 != unit || p2 != period) {
                fail("a freshly plugged wire is not immediately ready");
                return;
        }
        live[live_count++] = w;
}

static void op_unplug(void)
{
        wire_t w = pop_random_live();
        if (w == WIRE_NONE)
                return;
        object_unplug(w);
        if (base_wire_valid(w)) {
                fail("a departed wire is still valid");
                return;
        }
        u32 u;
        u64 p;
        if (object_declared(w, &u, &p)) {
                fail("a departed wire is still declared");
                return;
        }
        if (object_resolve(w, (motion_fixed_t[6]){0}) !=
                OBJECT_RESOLVE_E_NO_OBJECT) {
                fail("resolving a departed wire did not report NO_OBJECT");
                return;
        }
}

static void op_apply(void)
{
        if (live_count == 0)
                return;
        wire_t w = live[pick(live_count)];
        u32 dof = (u32)pick(MOTION_DOF_MAX + 2);          /* 0..7 */
        motion_event_t ev;
        ev.wire = w;
        ev.frame = pick(2) ? FRAME_WORLD
                           : (pop_random_live_or_forge());
        ev.issued_at_ns = pick(3) == 0 ? UINT64_MAX
                                       : (rnd() & 0xFFFFFFFFull);
        ev.duration_ns = pick(4) == 0 ? 0
                     : pick(4) == 1 ? UINT64_MAX
                     : (1 + pick(1 << 20));
        ev.dof = (u8)dof;               /* 0..7，包含越界 dof */
        for (u32 i = 0; i < MOTION_DOF_MAX; i++) {
                u64 k = pick(5);
                ev.motion[i] = k == 0 ? 0
                             : k == 1 ? Q(1)
                             : k == 2 ? INT64_MIN
                             : k == 3 ? INT64_MAX
                             : (motion_fixed_t)((s64)rnd() % Q(100));
        }
        base_apply((u64)rnd(), &ev);    /* 返回值本身已各分支合法 */
}

/*
 * 命令尾部不变量（设计决定 19）：事件只在 motion[0..dof-1] 上有分量，
 * 接受时必须把 dof 之外的分量归零。
 *
 * 这条不能只靠「读回来看看」：一个 dof=1 的物件若把尾部残值留在
 * cmd_motion 里，参照链会按 6 轴累加，把「没被命令的角速度」加进
 * 子物件的世界运动 —— 而命令本身看起来是对的。
 */
static void op_apply_tail(void)
{
        if (live_count == 0)
                return;
        wire_t w = live[pick(live_count)];
        u8 dof = (u8)pick(MOTION_DOF_MAX + 1);          /* 0..6 */

        motion_event_t ev;
        ev.wire = w;
        ev.frame = FRAME_WORLD;
        ev.issued_at_ns = 0;
        ev.duration_ns = 1000;
        ev.dof = dof;
        for (u32 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = (motion_fixed_t)((s64)(i + 1) * Q(1));

        if (base_apply(0, &ev) != MOTION_OK)
                return;                                  /* 被拒就不该写 */

        for (u32 i = 0; i < MOTION_DOF_MAX; i++) {
                motion_fixed_t got = object_motion(w, (u8)i);
                motion_fixed_t want = i < dof ? ev.motion[i] : 0;

                if (got != want) {
                        fail("a command left a value outside its declared dof");
                        return;
                }
        }
}

static wire_t pop_random_live_or_forge(void)
{
        if (live_count == 0)
                return FRAME_WORLD;
        return pick(3) == 0
                ? live[pick(live_count)]
                : WIRE_MAKE((u32)pick(1 << 24), (u32)(1 + pick(64)));
}

static void op_set_state(void)
{
        if (live_count == 0)
                return;
        wire_t w = live[pick(live_count)];
        object_state_t st;
        for (u8 i = 0; i < 3; i++) {
                st.position[i] = (motion_fixed_t)((s64)rnd() % Q(1000));
                st.linear[i]   = (motion_fixed_t)((s64)rnd() % Q(1000));
                st.angular[i]  = (motion_fixed_t)((s64)rnd() % Q(1000));
        }
        /* 身材各异的四元数：合法、未申报、缩放、彻底乱码 */
        u32 mode = (u32)pick(5);
        for (u8 i = 0; i < 4; i++)
                st.orientation[i] = (motion_fixed_t)rnd();
        if (mode == 0)
                for (u8 i = 0; i < 4; i++)
                        st.orientation[i] = 0;             /* 未申报 */
        else if (mode == 1)
                st.orientation[0] = Q(1);                  /* 单位 */
        else if (mode == 2)
                st.orientation[0] = Q(2);                  /* 缩放，非法 */

        object_state_t before, after;
        int had_before = object_state(w, &before);
        bool ok = object_set_state(w, &st);
        bool got_after = object_state(w, &after);

        if (!ok) {
                /* 被拒的姿态必须整份不写入 */
                if (got_after) {
                        for (u8 i = 0; i < 3; i++)
                                if (after.position[i] != before.position[i]) {
                                        fail("a refused pose wrote position");
                                        return;
                                }
                        for (u8 i = 0; i < 4; i++)
                                if (after.orientation[i] != before.orientation[i]) {
                                        fail("a refused pose wrote orientation");
                                        return;
                                }
                }
        } else {
                /* 被接受的姿态必须整份可读回 */
                if (!got_after)
                        return fail("an accepted pose is not readable");
                for (u8 i = 0; i < 3; i++)
                        if (after.position[i] != st.position[i])
                                return fail("accepted pose lost position");
                for (u8 i = 0; i < 4; i++)
                        if (after.orientation[i] != st.orientation[i])
                                return fail("accepted pose lost orientation");
        }
        (void)had_before;
}

static void op_describe(void)
{
        if (live_count == 0)
                return;
        wire_t w = live[pick(live_count)];
        object_capability_t cap;
        cap.dof = (u8)pick(MOTION_DOF_MAX + 1);
        cap.mass = (motion_fixed_t)((s64)rnd() - (s64)Q(10));
        for (u32 i = 0; i < MOTION_DOF_MAX; i++) {
                cap.limit_min[i] = i < cap.dof ? -(motion_fixed_t)Q(pick(5))
                                               : (motion_fixed_t)rnd();
                cap.limit_max[i] = i < cap.dof ? (motion_fixed_t)Q(pick(5))
                                               : (motion_fixed_t)rnd();
                cap.max_rate[i]  = i < cap.dof
                        ? (motion_fixed_t)((s64)pick(3) - 1) * Q(1)
                        : (motion_fixed_t)rnd();
        }
        object_capability_t before, back;
        bool had_before = object_capability(w, &before);
        bool ok = object_describe(w, &cap);
        bool readable = object_capability(w, &back);

        /*
         * 独立重算「这份申报合法吗」，而不是相信被测方的回答。
         * 判据与设计决定 20 一一对应；一旦被测方放宽任何一条，
         * 这里立刻出现分歧。
         */
        bool should_accept = !(cap.dof > MOTION_DOF_MAX || cap.mass < 0);

        for (u8 i = 0; i < cap.dof && i < MOTION_DOF_MAX; i++) {
                if (cap.limit_min[i] > cap.limit_max[i])
                        should_accept = false;
                if (cap.max_rate[i] < 0)
                        should_accept = false;
        }

        if (ok != should_accept)
                return fail("capability validation disagrees with the "
                            "documented rules");

        if (ok) {
                if (!readable)
                        return fail("an accepted capability is not readable");
                if (back.dof != cap.dof || back.mass != cap.mass)
                        return fail("accepted capability lost its declaration");
                for (u32 i = 0; i < MOTION_DOF_MAX; i++) {
                        if (i < cap.dof) {
                                if (back.limit_min[i] != cap.limit_min[i] ||
                                    back.limit_max[i] != cap.limit_max[i] ||
                                    back.max_rate[i] != cap.max_rate[i])
                                        return fail("accepted capability changed an active axis");
                        } else if (back.limit_min[i] != 0 ||
                                   back.limit_max[i] != 0 ||
                                   back.max_rate[i] != 0) {
                                return fail("accepted capability retained an undeclared tail");
                        }
                }
        } else {
                /* 拒绝新申报时，旧的合法申报必须原样保留。 */
                if (readable != had_before)
                        return fail("a refused capability changed readability");
                if (had_before) {
                        if (back.dof != before.dof || back.mass != before.mass)
                                return fail("a refused capability changed the old declaration");
                        for (u32 i = 0; i < MOTION_DOF_MAX; i++)
                                if (back.limit_min[i] != before.limit_min[i] ||
                                    back.limit_max[i] != before.limit_max[i] ||
                                    back.max_rate[i] != before.max_rate[i])
                                        return fail("a refused capability changed an old axis");
                }
        }
}

static void op_run(void)
{
        u64 now = pick(3) == 0 ? UINT64_MAX
                 : pick(3) == 0 ? 0
                 : (u64)rnd();
        u32 stepped = 999;
        object_run_status_t s = object_run(now, &stepped);
        if (s != OBJECT_RUN_OK && s != OBJECT_RUN_E_CLOCK_ROLLBACK &&
            s != OBJECT_RUN_E_TIME_OVERFLOW && s != OBJECT_RUN_E_INVAL)
                return fail("object_run returned an unknown status");
}

static void op_resolve(void)
{
        wire_t w = pick(2) ? pop_random_live_or_forge()
                           : WIRE_MAKE((u32)pick(1 << 24), (u32)(1 + pick(64)));
        motion_fixed_t out[6];
        object_resolve_status_t s = object_resolve(w, out);
        if (s != OBJECT_RESOLVE_OK && s != OBJECT_RESOLVE_E_INVAL &&
            s != OBJECT_RESOLVE_E_NO_OBJECT && s != OBJECT_RESOLVE_E_BROKEN_FRAME &&
            s != OBJECT_RESOLVE_E_CYCLE && s != OBJECT_RESOLVE_E_DEPTH &&
            s != OBJECT_RESOLVE_E_OVERFLOW)
                return fail("object_resolve returned an unknown status");
        /* 空输出必须是 INVAL（有活物件时）；绝不崩溃 */
        if (live_count) {
                if (object_resolve(live[0], 0) != OBJECT_RESOLVE_E_INVAL)
                        return fail("object_resolve(x, NULL) was not INVAL");
        }
}

int main(int argc, char **argv)
{
        unsigned long long iters = 2000000;
        if (argc > 1)
                iters = strtoull(argv[1], 0, 10);

        printf("\n=== 确定性模糊 / deterministic fuzz ===\n");
        printf("  seed %llu, %llu iterations\n", rng_state, iters);

        base_init();
        object_init();

        static const struct {
                const char *name;
                void (*fn)(void);
        } ops[] = {
                { "plug",        op_plug },
                { "unplug",      op_unplug },
                { "apply",       op_apply },
                { "tail",        op_apply_tail },
                { "set_state",   op_set_state },
                { "describe",    op_describe },
                { "run",         op_run },
                { "resolve",     op_resolve },
        };
        const u32 nops = (u32)(sizeof(ops) / sizeof(ops[0]));

        for (unsigned long long i = 0; i < iters; i++) {
                ops[pick(nops)].fn();
                if ((i & 0x3FF) == 0x3FF)
                        oracle();
        }
        oracle();

        printf("  fuzz iterations = %llu, random operations = %llu\n",
               iters, iters);
        printf("  fuzz failures = %llu\n", failures);
        return failures ? 1 : 0;
}