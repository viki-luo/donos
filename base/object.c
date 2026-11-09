/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * object.c — 物件侧的参考实现
 *
 * 这是「物件」那一侧的代码，不是基座的一部分。
 *
 * 关键分工：
 *   - 基座只知道「谁挂在哪个口」，把事件递过来
 *   - 物件知道自己在哪、在做什么，并自己保存这些
 *
 * 所以运动状态存在本文件里，而不是 base.c 里。这条分界就是设计本身：
 * 一旦运动状态搬进基座，基座就有状态了，整个模型随之崩塌。
 */
#include "base.h"
#include "fixed.h"
#include "motion_device.h"
#include "object.h"
#include "quat.h"
typedef struct {
        bool           in_use;
        wire_t         wire;

        /* --- 以下全部是物件自己的状态，基座一律看不到 --- */

        /*
         * 命令：本物件「相对 frame」的速度（线 0..2、角 3..5）。
         * 它不是世界速度 —— 世界速度要沿参照链解算（object_resolve），
         * 而解算结果不写回这里。命令与解算必须分开存：把它们合并成一个
         * 字段，就等于让「我被告知什么」和「我在世界里怎么动」互相覆盖。
         */
        motion_fixed_t cmd_motion[MOTION_DOF_MAX];
        wire_t         frame;
        u64            duration_ns;   /* 接时的新鲜度窗口（记录用） */
        u8             dof;
        u32            event_count;
        u32            unit;          /* 物件自述的单位语义（R3） */
        u64            period_ns;     /* 物件自述的控制周期，由执行层兑现 */
        motion_fixed_t carry[3];      /* 积分余数：不足一个最低位的零头 */
        motion_fixed_t orientation_carry[4]; /* 四元数各分量的积分余数 */
        u64            next_due_ns;   /* 下一次运动环到期时刻；0 = 尚未起步 */
        u32            step_count;

        /*
         * 实测/自持位姿。三者都在本物件自己的参照系里表达：
         *   position    相对 frame 的位置
         *   linear      相对 frame 的线速度（实测）
         *   angular     相对 frame 的角速度（实测）
         * 世界视角由 object_resolve() 给，不在这份状态里。
         */
        object_state_t      state;
        object_capability_t cap;
        bool                described;

        /*
         * 真实设备。非空时，位置与实测速度都来自 sample()，
         * 执行层不再自己积分 —— 设备才是唯一知道「实际动了多少」的一方。
         */
        const motion_device_ops_t *dev;
        void                      *dev_ctx;
} object_t;

static object_t objects[OBJECT_MAX];

/*
 * 执行层只保存调度时钟，用于拒绝 object_run() 时间回退。
 * 命令新鲜度不读这个值，而使用 base_apply() 显式传入的提交时刻。
 */
static u64 last_run_ns;
static bool run_clock_started;

/* 把一段内存清零，不依赖 libc（freestanding） */
static void zero(void *p, u64 n)
{
        u8 *b = (u8 *)p;
        for (u64 i = 0; i < n; i++)
                b[i] = 0;
}

/*
 * 物件侧初始化：清空物件表。
 * 理由同 base_init：不信任引导器会清 .bss。
 */
void object_init(void)
{
        last_run_ns = 0;
        run_clock_started = false;
        for (u64 i = 0; i < OBJECT_MAX; i++) {
                objects[i].in_use = false;
                objects[i].wire = WIRE_NONE;
                objects[i].frame = FRAME_WORLD;
                objects[i].duration_ns = 0;
                objects[i].dof = 0;
                objects[i].event_count = 0;
                objects[i].unit = 0;
                objects[i].period_ns = 0;
                objects[i].carry[0] = 0;
                objects[i].carry[1] = 0;
                objects[i].carry[2] = 0;
                for (u8 k = 0; k < 4; k++)
                        objects[i].orientation_carry[k] = 0;
                objects[i].next_due_ns = 0;
                objects[i].step_count = 0;
                for (u8 k = 0; k < MOTION_DOF_MAX; k++)
                        objects[i].cmd_motion[k] = 0;
                zero(&objects[i].state, sizeof(objects[i].state));
                zero(&objects[i].cap, sizeof(objects[i].cap));
                objects[i].described = false;
                objects[i].dev = 0;
                objects[i].dev_ctx = 0;
        }
}

static object_t *find_by_wire(wire_t w)
{
        for (u64 i = 0; i < OBJECT_MAX; i++)
                if (objects[i].in_use && objects[i].wire == w)
                        return &objects[i];
        return 0;
}

/* ------------------------------------------------------------------ */
/* 接入                                                                */
/* ------------------------------------------------------------------ */

/*
 * 物件插入基座：物件先申请一条接线，基座登记「谁挂在哪个口」。
 * 基座此时知道的唯一事实就是这条接线存在，此外一无所知。
 */
wire_t object_plug(u32 unit, u64 period_ns)
{
        /*
         * 周期上限属于执行模型契约。算术安全由 checked fixed-point 层
         * 独立保证，调度时间加法则在 object_run() 中逐次检查回绕。
         */
        if (period_ns > OBJECT_PERIOD_NS_MAX)
                return WIRE_NONE;

        for (u64 i = 0; i < OBJECT_MAX; i++) {
                if (objects[i].in_use)
                        continue;

                /*
                 * 两阶段发布：先把除接线以外的每个字段都初始化好，
                 * 再向基座申请接线，最后才把接线写进去。
                 *
                 * 顺序就是正确性。旧实现先写 wire，再写 period_ns、frame、
                 * cmd_motion、state、cap —— 而 find_by_wire() 只认
                 * 「in_use 且 wire 相符」，于是那个窗口里这条接线已经
                 * 可以收命令，而被判定所依据的 frame、速率上限、容量
                 * 还是**上一个占用者**的值。命令不会失败，它会按错误的
                 * 参照系被接受 —— 比拒绝危险得多。
                 *
                 * 现在接线是最后一个被写入的字段，因此「接线可见」与
                 * 「状态就绪」是同一个瞬间。
                 */
                objects[i].frame = FRAME_WORLD;
                objects[i].duration_ns = 0;
                objects[i].dof = 0;
                objects[i].event_count = 0;
                objects[i].unit = unit;
                objects[i].period_ns = period_ns;
                objects[i].carry[0] = 0;
                objects[i].carry[1] = 0;
                objects[i].carry[2] = 0;
                for (u8 k = 0; k < 4; k++)
                        objects[i].orientation_carry[k] = 0;
                objects[i].next_due_ns = 0;
                objects[i].step_count = 0;
                for (u8 k = 0; k < MOTION_DOF_MAX; k++)
                        objects[i].cmd_motion[k] = 0;
                zero(&objects[i].state, sizeof(objects[i].state));
                zero(&objects[i].cap, sizeof(objects[i].cap));
                objects[i].described = false;
                objects[i].dev = 0;
                objects[i].dev_ctx = 0;

                wire_t w = base_arrive();
                if (w == WIRE_NONE)
                        return WIRE_NONE;   /* 槽位未被占用，状态写回无害 */

                objects[i].wire = w;
                objects[i].in_use = true;   /* 发布点：此后才可被找到 */

                return w;
        }
        return WIRE_NONE;
}

void object_unplug(wire_t w)
{
        object_t *o = find_by_wire(w);
        if (o == 0)
                return;

        /*
         * 先让物件不可再被找到，再交还接线。
         *
         * 反过来的话，接线已经失效而物件仍可被 find_by_wire 命中，
         * 那是一个「基座说没有、物件说有」的矛盾窗口。
         */
        o->in_use = false;
        o->wire = WIRE_NONE;
        base_depart(w);
}

/* ------------------------------------------------------------------ */
/* 基座的调用点                                                        */
/* ------------------------------------------------------------------ */

/*
 * 基座把已经校验过的事件递到这里。物件接住它，并自己负责后续 ——
 * 包括接之后继续沿轨道运动。
 *
 * 新鲜度判定（设计决定 10 修订）在这里做，而不是在基座里：
 * 事件只在 [issued_at_ns, issued_at_ns + duration_ns] 窗口内可被接受。
 * now_ns 是事件本次到达入口的提交时刻，只在本次调用中使用；基座与
 * 物件都不把它保存成世界时钟，也不复用 object_run() 的旧调度节拍。
 */
/*
 * 幅值是否超过申报上限。上限为 0 表示该轴未申报，永不设限。
 *
 * INT64_MIN 的幅值是 2^63，任何 int64 正上限都拦不住它，而直接取负会
 * 在 INT64_MIN 上溢出，所以先把这一支单独判掉。
 */
static bool exceeds_rate(motion_fixed_t v, motion_fixed_t limit)
{
        if (limit <= 0)
                return false;
        if (v == INT64_MIN)
                return true;
        return (v < 0 ? -v : v) > limit;
}

static bool within_declared_rate(const object_t *o, const motion_event_t *ev)
{
        for (u8 i = 0; i < ev->dof && i < MOTION_DOF_MAX; i++)
                if (exceeds_rate(ev->motion[i], o->cap.max_rate[i]))
                        return false;
        return true;
}

motion_status_t object_submit(wire_t w, u64 now_ns,
                              const motion_event_t *ev)
{
        object_t *o = find_by_wire(w);
        if (o == 0)
                return MOTION_E_NO_OBJECT;

        if (ev->duration_ns == 0)
                return MOTION_E_INVAL;            /* 零长度窗口永远无效 */

        u64 deadline = ev->issued_at_ns + ev->duration_ns;
        if (deadline < ev->issued_at_ns)
                return MOTION_E_INVAL;            /* 窗口端点回绕 */

        if (ev->issued_at_ns > now_ns)
                return MOTION_E_INVAL;            /* 未来事件：时间来源异常 */

        if (now_ns > deadline)
                return MOTION_E_DEADLINE;         /* 过期：拒绝陈旧命令 */

        /*
         * 能力闸门：物件只接受自己申报能力范围内的速率。
         *
         * 这一步在物件侧，不在基座里 —— 基座不读能力申报，也无从知道
         * max_rate 是什么。max_rate 为 0 表示「未申报该轴」，此时不设限；
         * 申报了却不执行，申报就是装饰，与 duration_ns 曾经的下场一样。
         */
        if (o->described && !within_declared_rate(o, ev))
                return MOTION_E_LIMIT;

        o->dof = ev->dof;
        o->frame = ev->frame;
        o->duration_ns = ev->duration_ns;

        /*
         * dof 尾部归零，不是「照抄全部 6 个分量」。
         *
         * 事件只声明了 dof 个分量，其余位置不是这条命令的一部分：调用者
         * 完全可能只填自己声明的那几个，尾部留着上一条命令的残值或未初始化
         * 的栈内容。照抄会把「没被命令的运动」带进模型，而且它会顺着参照链
         * 累加进世界速度（object_resolve 按 6 轴累加），一个 dof=1 的物件
         * 因此能悄悄推动整条链的角速度。
         */
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                o->cmd_motion[i] = i < ev->dof ? ev->motion[i] : 0;
        o->event_count++;

        return MOTION_OK;
}

/* ------------------------------------------------------------------ */
/* 自检用的只读查询                                                    */
/* ------------------------------------------------------------------ */

/*
 * 本物件被告知的运动是否全零。
 *
 * 名字刻意不叫 object_is_still：它读的是**命令**，相对 frame 表达。
 * 「命令为零」只说明「本物件不相对 frame 动」，不说明它在世界里静止 ——
 * 参照系自己在动，它照样在世界里走（静止是一种相对的运动）。
 * 世界的动与不动只能由 object_resolve() 的结果判断，而那属于大脑。
 */
bool object_commanded_is_zero(wire_t w)
{
        object_t *o = find_by_wire(w);
        if (o == 0)
                return false;

        for (u8 i = 0; i < o->dof; i++)
                if (o->cmd_motion[i] != 0)
                        return false;
        return true;
}

u32 object_event_count(wire_t w)
{
        object_t *o = find_by_wire(w);
        return o == 0 ? 0 : o->event_count;
}

motion_fixed_t object_motion(wire_t w, u8 i)
{
        object_t *o = find_by_wire(w);
        if (o == 0 || i >= MOTION_DOF_MAX)
                return 0;
        return o->cmd_motion[i];
}

/* ------------------------------------------------------------------ */
/* R8：物件自述与自持                                                  */
/*                                                                    */
/* 全部逐字段拷贝，不做整结构赋值 —— 大结构赋值可能让编译器调用        */
/* memcpy，而 freestanding 下没有链接 libc。                          */
/* ------------------------------------------------------------------ */

bool object_describe(wire_t w, const object_capability_t *cap)
{
        object_t *o = find_by_wire(w);
        if (o == 0 || cap == 0)
                return false;

        if (cap->dof > MOTION_DOF_MAX || cap->mass < 0)
                return false;

        for (u8 i = 0; i < cap->dof; i++) {
                if (cap->limit_min[i] > cap->limit_max[i])
                        return false;           /* 区间反向 */
                if (cap->max_rate[i] < 0)
                        return false;           /* 负速率无意义 */
        }

        /*
         * 校验通过才写入，且 dof 之外的轴按「未申报」清零。
         *
         * 校验失败时不改动任何已有记录：一份被悄悄打折的申报比没有申报
         * 更危险，而静默接受非法申报正是死字段的来源。
         */
        o->cap.dof = cap->dof;
        o->cap.mass = cap->mass;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++) {
                if (i < cap->dof) {
                        o->cap.limit_min[i] = cap->limit_min[i];
                        o->cap.limit_max[i] = cap->limit_max[i];
                        o->cap.max_rate[i]  = cap->max_rate[i];
                } else {
                        o->cap.limit_min[i] = 0;
                        o->cap.limit_max[i] = 0;
                        o->cap.max_rate[i]  = 0;
                }
        }
        o->described = true;
        return true;
}

bool object_capability(wire_t w, object_capability_t *out)
{
        object_t *o = find_by_wire(w);
        if (o == 0 || out == 0 || !o->described)
                return false;

        out->dof = o->cap.dof;
        out->mass = o->cap.mass;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++) {
                out->limit_min[i] = o->cap.limit_min[i];
                out->limit_max[i] = o->cap.limit_max[i];
                out->max_rate[i]  = o->cap.max_rate[i];
        }
        return true;
}

/*
 * 四元数是否可当作旋转使用。
 *
 * 全零不算「非法」，而是「未申报姿态」：物件从没调用过
 * object_set_state 时 orientation 全是零，那不是一条错误的旋转，
 * 而是没有旋转可报。把它判成错误，会让每一个还没上报姿态的物件
 * 都无法参与解算 —— 那不是严格，那是把一个正常状态当故障。
 *
 * 非零就必须是单位四元数：|q|² 必须落在 1 ± 1/1024 之内。取平方和
 * 而不是逐个比较，是因为整体缩放的四元数（例如 2q）方向正确但不是
 * 旋转 —— 用它旋转会得到两倍长度的向量，而错误会伪装成精度问题。
 * 容差 1/1024 是给 Q32.32 存储舍入留的余量，不是给「大致归一」留的。
 */
#define Q_ONE        4294967296LL          /* 1.0 in Q32.32 */
#define Q_UNIT_TOL   (Q_ONE / 1024)

static bool quaternion_is_rotation(const motion_fixed_t q[4], bool *undeclared)
{
        bool all_zero = true;
        motion_fixed_t sum = 0;

        for (u8 i = 0; i < 4; i++)
                if (q[i] != 0)
                        all_zero = false;

        if (undeclared != 0)
                *undeclared = all_zero;
        if (all_zero)
                return true;

        for (u8 i = 0; i < 4; i++) {
                motion_fixed_t sq;

                if (fixed_mul_checked(q[i], q[i], &sq) != FIXED_OK ||
                    fixed_add_checked(sum, sq, &sum) != FIXED_OK)
                        return false;
        }

        motion_fixed_t dev;

        if (fixed_sub_checked(sum, Q_ONE, &dev) != FIXED_OK)
                return false;
        if (dev == INT64_MIN)
                return false;
        if (dev < 0)
                dev = -dev;

        return dev <= Q_UNIT_TOL;
}

bool object_set_state(wire_t w, const object_state_t *st)
{
        object_t *o = find_by_wire(w);
        if (o == 0 || st == 0)
                return false;

        /*
         * 姿态在入口校验，且**整体拒绝**：非法四元数不写入任何字段。
         *
         * 只写位置、丢掉姿态是不行的 —— 调用者以为自己上报了一份位姿，
         * 而模型里留下的是「位置是新的、姿态是旧的」这种从未存在过的
         * 组合。要么整份收下，要么整份退回，与能力申报同一条纪律。
         */
        if (!quaternion_is_rotation(st->orientation, 0))
                return false;

        for (u8 i = 0; i < 3; i++) {
                o->state.position[i] = st->position[i];
                o->state.linear[i]   = st->linear[i];
                o->state.angular[i]  = st->angular[i];
        }
        for (u8 i = 0; i < 4; i++)
                o->state.orientation[i] = st->orientation[i];

        /*
         * 位姿被人为改写之后，旧的积分余数必须丢掉。
         *
         * 余数是「上一条轨迹还没补上的零头」，它属于被改写前的那个位置。
         * 留着它，下一次积分会把它加到新位置上 —— 新轨迹凭空多出
         * 一个不足最低位的偏移，而且这个偏移取决于改写前跑了多久，
         * 于是「同样的命令、同样的位置」给出不同的结果。
         */
        for (u8 i = 0; i < 3; i++)
                o->carry[i] = 0;
        for (u8 i = 0; i < 4; i++)
                o->orientation_carry[i] = 0;

        return true;
}

bool object_state(wire_t w, object_state_t *out)
{
        object_t *o = find_by_wire(w);
        if (o == 0 || out == 0)
                return false;

        for (u8 i = 0; i < 3; i++) {
                out->position[i] = o->state.position[i];
                out->linear[i]   = o->state.linear[i];
                out->angular[i]  = o->state.angular[i];
        }
        for (u8 i = 0; i < 4; i++)
                out->orientation[i] = o->state.orientation[i];
        return true;
}

/* ------------------------------------------------------------------ */
/* 参照系解算（方案 A：物件自己解，物件间直连）                         */
/* ------------------------------------------------------------------ */

/*
 * 有符号 128 位「除以 2^32 并就近取整」。
 *
 * 负数不直接取负（那会在 INT128_MIN 上溢出），而是先取幅值再回填符号，
 * 因此两侧取整完全对称。2^32 是编译期常量，右移不拉进编译器运行库。
 *
 * 为什么不直接用 >> 32：算术右移对负数是向 −∞ 取整。参照系旋转会
 * 处理反向分量，向 −∞ 取整在该侧恒偏小，等于给旋转加了单向偏差。
 */
/*
 * 用单位四元数 q = {w, x, y, z} 旋转三维向量 v：
 *     t  = 2 (u × v)
 *     v' = v + w t + u × t        其中 u = (x, y, z)
 *
 * 所有定点运算都经过 checked 层。任一中间值超出 Q32.32 表示范围时
 * 返回 false，调用者不得使用部分计算结果。
 * 全零四元数表示「未申报姿态」，按无旋转处理；非零姿态在
 * object_set_state() 入口已校验为单位四元数，因此这里不再重复校验。
 */
static bool rotate3(const motion_fixed_t q[4], motion_fixed_t v[3])
{
        motion_fixed_t w = q[0], x = q[1], y = q[2], z = q[3];
        motion_fixed_t yv2, zv1, zv0, xv2, xv1, yv0;
        motion_fixed_t cross0, cross1, cross2;
        motion_fixed_t t0, t1, t2;
        motion_fixed_t wt0, yt2, zt1;
        motion_fixed_t wt1, zt0, xt2;
        motion_fixed_t wt2, xt1, yt0;
        motion_fixed_t sum, next[3];

        if (q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 0)
                return true;

        if (fixed_mul_checked(y, v[2], &yv2) != FIXED_OK ||
            fixed_mul_checked(z, v[1], &zv1) != FIXED_OK ||
            fixed_sub_checked(yv2, zv1, &cross0) != FIXED_OK ||
            fixed_mul2_checked(cross0, &t0) != FIXED_OK ||
            fixed_mul_checked(z, v[0], &zv0) != FIXED_OK ||
            fixed_mul_checked(x, v[2], &xv2) != FIXED_OK ||
            fixed_sub_checked(zv0, xv2, &cross1) != FIXED_OK ||
            fixed_mul2_checked(cross1, &t1) != FIXED_OK ||
            fixed_mul_checked(x, v[1], &xv1) != FIXED_OK ||
            fixed_mul_checked(y, v[0], &yv0) != FIXED_OK ||
            fixed_sub_checked(xv1, yv0, &cross2) != FIXED_OK ||
            fixed_mul2_checked(cross2, &t2) != FIXED_OK)
                return false;

        if (fixed_mul_checked(w, t0, &wt0) != FIXED_OK ||
            fixed_mul_checked(y, t2, &yt2) != FIXED_OK ||
            fixed_mul_checked(z, t1, &zt1) != FIXED_OK ||
            fixed_add_checked(v[0], wt0, &sum) != FIXED_OK ||
            fixed_add_checked(sum, yt2, &sum) != FIXED_OK ||
            fixed_sub_checked(sum, zt1, &next[0]) != FIXED_OK ||
            fixed_mul_checked(w, t1, &wt1) != FIXED_OK ||
            fixed_mul_checked(z, t0, &zt0) != FIXED_OK ||
            fixed_mul_checked(x, t2, &xt2) != FIXED_OK ||
            fixed_add_checked(v[1], wt1, &sum) != FIXED_OK ||
            fixed_add_checked(sum, zt0, &sum) != FIXED_OK ||
            fixed_sub_checked(sum, xt2, &next[1]) != FIXED_OK ||
            fixed_mul_checked(w, t2, &wt2) != FIXED_OK ||
            fixed_mul_checked(x, t1, &xt1) != FIXED_OK ||
            fixed_mul_checked(y, t0, &yt0) != FIXED_OK ||
            fixed_add_checked(v[2], wt2, &sum) != FIXED_OK ||
            fixed_add_checked(sum, xt1, &sum) != FIXED_OK ||
            fixed_sub_checked(sum, yt0, &next[2]) != FIXED_OK)
                return false;

        v[0] = next[0];
        v[1] = next[1];
        v[2] = next[2];
        return true;
}

/*
 * 三维叉积，全程 checked：ω × r 的两个乘积之差在定点下同样会越界。
 *   t0 = a1 b2 - a2 b1
 *   t1 = a2 b0 - a0 b2
 *   t2 = a0 b1 - a1 b0
 */
static bool cross3_checked(const motion_fixed_t a[3],
                           const motion_fixed_t b[3],
                           motion_fixed_t out[3])
{
        static const u8 li[3] = { 1, 2, 0 };   /* 被减项的下标 */
        static const u8 ri[3] = { 2, 0, 1 };

        for (u8 i = 0; i < 3; i++) {
                motion_fixed_t p, q;

                if (fixed_mul_checked(a[li[i]], b[ri[i]], &p) != FIXED_OK ||
                    fixed_mul_checked(a[ri[i]], b[li[i]], &q) != FIXED_OK ||
                    fixed_sub_checked(p, q, &out[i]) != FIXED_OK)
                        return false;
        }
        return true;
}

object_resolve_status_t object_resolve(wire_t w,
                                       motion_fixed_t out[MOTION_DOF_MAX])
{
        object_t *o = find_by_wire(w);
        if (o == 0)
                return OBJECT_RESOLVE_E_NO_OBJECT;
        if (out == 0)
                return OBJECT_RESOLVE_E_INVAL;

        /*
         * 循环不变量：v、wv、r 三者都表达在「当前参照系」的坐标里，
         * 其中 r 是本物件相对当前参照系原点的位置。
         */
        motion_fixed_t v[3];
        motion_fixed_t wv[3];
        motion_fixed_t r[3];
        bool r_nonzero = false;

        /*
         * 走过的接线。成环与「链太深」是两件事，但它们此前共用同一个
         * `depth >= MAX` 判定，于是任何长得合法但太深的链都被报成环 ——
         * 诊断信息直接说谎，而且真实成环与需要缩短链的现场无法区分。
         * 记住走过的接线，重复出现才是环。
         */
        wire_t seen[OBJECT_FRAME_DEPTH_MAX];

        for (u8 i = 0; i < 3; i++) {
                v[i]  = o->cmd_motion[i];         /* 命令：相对 frame 的线速度 */
                wv[i] = o->cmd_motion[3 + i];     /* 命令：相对 frame 的角速度 */
                r[i]  = o->state.position[i];     /* 实测：相对 frame 的位置 */
                if (r[i] != 0)
                        r_nonzero = true;
        }

        wire_t frame = o->frame;
        for (u32 depth = 0; frame != FRAME_WORLD; depth++) {
                if (depth >= OBJECT_FRAME_DEPTH_MAX)
                        return OBJECT_RESOLVE_E_DEPTH;  /* 合法但过长 */

                for (u32 k = 0; k < depth; k++)
                        if (seen[k] == frame)
                                return OBJECT_RESOLVE_E_CYCLE;

                seen[depth] = frame;

                object_t *f = find_by_wire(frame);
                if (f == 0)
                        return OBJECT_RESOLVE_E_BROKEN_FRAME;

                const motion_fixed_t *q = f->state.orientation;

                /*
                 * 先把本级三个量按参照系姿态转到它的上一级坐标。
                 * 参照系的 zero 四元数由 rotate3 当作「无旋转」处理。
                 */
                if (!rotate3(q, v) || !rotate3(q, wv))
                        return OBJECT_RESOLVE_E_OVERFLOW;
                if (r_nonzero && !rotate3(q, r))
                        return OBJECT_RESOLVE_E_OVERFLOW;

                /*
                 * 位置耦合项 ω_F × r。
                 *
                 * 少了这一项，「挂在转动的参照系上的物件」就只会跟着平移，
                 * 不会被转出去：参照系以 1 rad/s 自转、物件在半径 1 m 处，
                 * 它相对世界的线速度里必须有 1 m/s 的切向分量。此前只做
                 * 「旋转命令 + 叠加参照系命令」，这个分量整个丢失。
                 *
                 * r 用**实测**位置而不是命令：耦合项取决于物件实际待在
                 * 哪里，不取决于它被告知该往哪儿走。
                 */
                motion_fixed_t tang[3] = { 0, 0, 0 };

                const motion_fixed_t *wf = &f->cmd_motion[3];
                if (r_nonzero && !cross3_checked(wf, r, tang))
                        return OBJECT_RESOLVE_E_OVERFLOW;

                for (u8 i = 0; i < 3; i++) {
                        motion_fixed_t sum;

                        if (fixed_add_checked(v[i], tang[i], &sum) != FIXED_OK ||
                            fixed_add_checked(sum, f->cmd_motion[i], &v[i])
                                    != FIXED_OK)
                                return OBJECT_RESOLVE_E_OVERFLOW;

                        if (fixed_add_checked(wv[i], wf[i], &wv[i]) != FIXED_OK)
                                return OBJECT_RESOLVE_E_OVERFLOW;
                }

                /*
                 * 本级原点相对上一级原点也有一段位移，跟着一起上移。
                 * 不累加它，上一级的旋转就会绕着错误的原点转。
                 */
                for (u8 i = 0; i < 3; i++) {
                        if (fixed_add_checked(r[i], f->state.position[i],
                                              &r[i]) != FIXED_OK)
                                return OBJECT_RESOLVE_E_OVERFLOW;
                        if (r[i] != 0)
                                r_nonzero = true;
                }

                frame = f->frame;
        }

        for (u8 i = 0; i < 3; i++) {
                out[i]     = v[i];
                out[3 + i] = wv[i];
        }
        return OBJECT_RESOLVE_OK;
}

/* ------------------------------------------------------------------ */
/* 姿态积分与行程边界（物件侧）                                         */
/*                                                                    */
/* 这一层在基座之外。基座不调度、不计时；时间由调用者（架构层）给出。 */
/* ------------------------------------------------------------------ */

/*
 * 物件的一步运动环：把本物件相对 frame 的命令速度积分进相对位姿。
 *
 * cmd_motion 是速度（设计决定 18）：一步积分「速度 × 自述周期",
 * 与周期长短无关地逼近同一世界速度。落后时不补跑（决定 16），
 * 因此这里也只积分一个周期，不积实际流失的时间。
 *
 * 积分用「余数进位」，不是每步各自取整：
 *
 *     acc  = v * period_ns                     （精确，不缺位）
 *     step = acc / 1e9      + carry[i]         （本位）
 *     carry[i] = acc % 1e9                     （不足一位的余数留到下一步）
 *
 * 每步各自取整时，舍入误差恒为同号，会随步数线性累积：同样 3 m/s、
 * 同样 9 ms，1 ms 周期与 3 ms 周期会差出 9 个最低位，且周期越细
 * 误差越大——那不是「精度问题」，而是速度本身被周期改写了。
 * 余数进位把丢掉的零头存下来在下一次补上，误差只在 ±1 个最低位之间
 * 摆动而不累积，因此两条不同周期的积分路径给出逐比特相同的结果。
 *
 * 只积分线分量（0..2）到相对位置；角分量（3..5）在 dof=6 的命令下
 * 按 q̇ = ½ ω ⊗ q 积分进姿态并逐步归一化。ω 按参照系坐标表达，
 * 与 object_resolve 沿链旋转命令角速度的约定一致（q 把本体坐标转到
 * 参照系坐标，故用左乘）。姿态未申报（全零）而角速度非零时，从单位
 * 姿态开始旋转。
 *
 * 整步原子性：位置、姿态、余数先全部算进临时值，任一轴积分失败、
 * 姿态归一化失败或行程限位拒绝，整步一起拒绝——物件停在原位原姿态，
 * 不留下「位置动了、姿态没动」的半步。
 *
 * 行程限位：申报了 [limit_min, limit_max]（两者不同时为 0）的轴，
 * 积分结果落在区间外即拒绝该步。物件停在最后一次界内位置；
 * 该步仍计入执行（调度前进），与参照链断开时的处理一致。
 * 参照链断开或成环时位置保持不动，但这一步仍算执行过。
 */
static bool integrate_orientation(object_t *o)
{
        motion_fixed_t q[4];
        motion_fixed_t omega[3];
        motion_fixed_t product[4];
        motion_fixed_t next_carry[4];
        bool rotating = false;

        if (o->dof < 6)
                return true;
        for (u8 i = 0; i < 3; i++) {
                omega[i] = o->cmd_motion[3 + i];
                if (omega[i] != 0)
                        rotating = true;
        }
        for (u8 i = 0; i < 4; i++) {
                q[i] = o->state.orientation[i];
                next_carry[i] = o->orientation_carry[i];
        }
        if (!rotating)
                return true;
        /* 从未申报姿态的物件从单位姿态开始旋转。 */
        if (q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 0)
                q[0] = Q_ONE;

        /*
         * 命令角速度按参照系坐标表达（object_resolve 沿链旋转的正是它），
         * q 把本体坐标转到参照系坐标，因此 q̇ = ½ ω ⊗ q，用左乘。
         */
        if (!quat_angular_product(q, omega, product))
                return false;

        for (u8 i = 0; i < 4; i++) {
                motion_fixed_t derivative, delta;
                if (fixed_mul_checked(product[i], Q_ONE / 2, &derivative) != FIXED_OK ||
                    fixed_velocity_step_checked(derivative, o->period_ns,
                                                o->orientation_carry[i], &delta,
                                                &next_carry[i]) != FIXED_OK ||
                    fixed_add_checked(q[i], delta, &q[i]) != FIXED_OK)
                        return false;
        }
        if (!quat_normalize(q))
                return false;
        for (u8 i = 0; i < 4; i++) {
                o->state.orientation[i] = q[i];
                o->orientation_carry[i] = next_carry[i];
        }
        return true;
}

/*
 * 挂/摘真实设备。
 *
 * 挂上设备之后，位置与实测速度不再由执行层自己推算：设备才是唯一
 * 知道「实际动了多少」的一方。这也是「命令 / 解算 / 实测」三分离的
 * 最后一段 —— 之前实测只是命令的影子，偏差检测因此永远为零。
 */
bool object_attach_device(wire_t w, const void *ops, void *ctx)
{
        object_t *o = find_by_wire(w);
        if (o == 0)
                return false;

        /*
         * 只有 ops == 0 表示摘除。ctx 允许为 0：无状态设备是合法实现，
         * 把「没有上下文」当成「摘除设备」会让这类设备根本挂不上。
         */
        if (ops == 0) {
                o->dev = 0;
                o->dev_ctx = 0;
                return true;
        }

        o->dev = (const motion_device_ops_t *)ops;
        o->dev_ctx = ctx;
        return true;
}

void object_detach_device(wire_t w)
{
        object_t *o = find_by_wire(w);
        if (o == 0)
                return;
        o->dev = 0;
        o->dev_ctx = 0;
}

bool object_has_device(wire_t w)
{
        object_t *o = find_by_wire(w);
        return o != 0 && o->dev != 0;
}

static void object_step(object_t *o)
{
        motion_fixed_t r[MOTION_DOF_MAX];

        /*
         * 真实设备模式：命令交给设备，实测从设备读回。
         *
         * 这里刻意不做任何积分 —— 真实电机的位置由编码器说了算，
         * 执行层再积一次就等于把同一段位移记两遍。
         * 读不到反馈时保持上一次的实测值：把它当成零会让监视器
         * 以为物件「已经停住」，那是比没有数据更坏的谎话。
         */
        if (o->dev != 0) {
                motion_fixed_t linear[3];
                motion_fixed_t angular[3];

                for (u8 i = 0; i < 3; i++) {
                        linear[i]  = o->cmd_motion[i];
                        angular[i] = o->cmd_motion[3 + i];
                }

                if (o->dev->set_velocity != 0)
                        (void)o->dev->set_velocity(o->dev_ctx, linear, angular);

                if (o->dev->sample != 0) {
                        object_state_t sampled;

                        /*
                         * 逐字段拷贝，不做整结构赋值：整结构赋值会让编译器
                         * 生成 memcpy，而 freestanding 构建没有 libc，
                         * 链接期就会缺符号。
                         */
                        if (o->dev->sample(o->dev_ctx, &sampled)) {
                                for (u8 i = 0; i < 3; i++) {
                                        o->state.position[i] = sampled.position[i];
                                        o->state.linear[i]   = sampled.linear[i];
                                        o->state.angular[i]  = sampled.angular[i];
                                }
                                for (u8 i = 0; i < 4; i++)
                                        o->state.orientation[i] =
                                                sampled.orientation[i];
                        }
                }

                o->step_count++;
                return;
        }

        /*
         * 解算在这里的作用是**参照链存活检查**，不是取速度来源。
         *
         * 积分必须用本物件自己相对 frame 的命令速度：它推进的是自己
         * 相对参照系的位姿。若改成把解算出的世界速度积分进一个相对
         * 参照系的 position，量纲就错了 —— 参照系自己在动时，物件的
         * 相对位姿会凭空长出参照系的位移。
         *
         * 解算仍然要跑：链断掉或成环时物件没有可用的参照系，
         * 此时位姿保持不动（这一步仍算执行过）。
         */
        if (object_resolve(o->wire, r) == OBJECT_RESOLVE_OK) {
                motion_fixed_t next_position[3];
                motion_fixed_t next_carry[3];
                u8 count = o->dof < 3 ? o->dof : 3;
                bool valid = true;

                /*
                 * 先在临时值中计算整步，全部轴都成功后再一次性提交。
                 * 这样任一速度积分或位置累计溢出时，物件不会留下
                 * 「两个轴已更新、第三个轴失败」的半步状态。
                 */
                for (u8 i = 0; i < count; i++) {
                        motion_fixed_t delta;

                        if (fixed_velocity_step_checked(o->cmd_motion[i],
                                                        o->period_ns,
                                                        o->carry[i], &delta,
                                                        &next_carry[i]) != FIXED_OK ||
                            fixed_add_checked(o->state.position[i], delta,
                                              &next_position[i]) != FIXED_OK) {
                                valid = false;
                                break;
                        }
                        /* Non-zero declared intervals are enforced at the
                         * integration boundary. [0,0] retains the legacy
                         * "no declared limit" meaning for compatibility. */
                        if (o->described && i < o->cap.dof &&
                            (o->cap.limit_min[i] != 0 || o->cap.limit_max[i] != 0) &&
                            (next_position[i] < o->cap.limit_min[i] ||
                             next_position[i] > o->cap.limit_max[i]))
                                valid = false;
                }

                if (valid && !integrate_orientation(o))
                        valid = false;
                if (valid) {
                        for (u8 i = 0; i < count; i++) {
                                o->state.position[i] = next_position[i];
                                o->carry[i] = next_carry[i];
                        }
                }
        }

        /*
         * 参考实现（无真实设备）：控制环就是物件自己，于是「刚被命令的
         * 速度」就是「当前实测的速度」。这条路径只用于模拟与自检；
         * 真实设备走上面的分支，实测必须来自传感器。
         */
        for (u8 i = 0; i < 3; i++) {
                o->state.linear[i]  = o->cmd_motion[i];
                o->state.angular[i] = o->cmd_motion[3 + i];
        }

        o->step_count++;
}

static bool time_add_checked(u64 a, u64 b, u64 *out)
{
        if (out == 0 || a > UINT64_MAX - b)
                return false;
        *out = a + b;
        return true;
}

object_run_status_t object_run(u64 now_ns, u32 *stepped)
{
        u64 next_due[OBJECT_MAX];

        if (stepped == 0)
                return OBJECT_RUN_E_INVAL;
        *stepped = 0;

        if (run_clock_started && now_ns < last_run_ns)
                return OBJECT_RUN_E_CLOCK_ROLLBACK;

        /*
         * 第一遍只计算排期，不修改任何物件。这样即使后面的物件发生
         * 时间回绕，整个 object_run 也不会留下前半张表已经推进的状态。
         */
        for (u64 i = 0; i < OBJECT_MAX; i++) {
                object_t *o = &objects[i];

                next_due[i] = o->next_due_ns;
                if (!o->in_use || o->period_ns == 0)
                        continue;

                if (o->next_due_ns == 0) {
                        if (!time_add_checked(now_ns, o->period_ns,
                                              &next_due[i]))
                                return OBJECT_RUN_E_TIME_OVERFLOW;
                        continue;       /* 首次只排期，不凭空积分一个周期 */
                }

                if (now_ns < o->next_due_ns)
                        continue;

                if (!time_add_checked(o->next_due_ns, o->period_ns,
                                      &next_due[i]))
                        return OBJECT_RUN_E_TIME_OVERFLOW;

                /* 落后时不补跑：从当前时刻重新排下一次。 */
                if (next_due[i] <= now_ns &&
                    !time_add_checked(now_ns, o->period_ns, &next_due[i]))
                        return OBJECT_RUN_E_TIME_OVERFLOW;
        }

        /* 第二遍才提交排期与运动步，前一遍已证明所有时间加法安全。 */
        for (u64 i = 0; i < OBJECT_MAX; i++) {
                object_t *o = &objects[i];

                if (!o->in_use || o->period_ns == 0)
                        continue;

                if (o->next_due_ns == 0) {
                        o->next_due_ns = next_due[i];
                        continue;
                }

                if (now_ns < o->next_due_ns)
                        continue;

                object_step(o);
                o->next_due_ns = next_due[i];
                (*stepped)++;
        }

        last_run_ns = now_ns;
        run_clock_started = true;
        return OBJECT_RUN_OK;
}

u64 object_now_ns(void)
{
        return last_run_ns;
}

u32 object_step_count(wire_t w)
{
        object_t *o = find_by_wire(w);
        return o == 0 ? 0 : o->step_count;
}

bool object_declared(wire_t w, u32 *unit, u64 *period_ns)
{
        object_t *o = find_by_wire(w);
        if (o == 0 || unit == 0 || period_ns == 0)
                return false;

        *unit = o->unit;
        *period_ns = o->period_ns;
        return true;
}