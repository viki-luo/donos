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
#include "object.h"

typedef struct {
        bool           in_use;
        wire_t         wire;

        /* --- 以下全部是物件自己的状态，基座一律看不到 --- */
        motion_fixed_t motion[MOTION_DOF_MAX];
        wire_t         frame;
        u64            duration_ns;   /* 接时的新鲜度窗口（记录用） */
        u8             dof;
        u32            event_count;
        u32            unit;          /* 物件自述的单位语义（R3） */
        u64            period_ns;     /* 物件自述的控制周期，由执行层兑现 */
        motion_fixed_t carry[3];      /* 积分余数：不足一个最低位的零头 */
        u64            next_due_ns;   /* 下一次运动环到期时刻；0 = 尚未起步 */
        u32            step_count;

        /* --- R8：物件自述与自持 --- */
        object_state_t      state;
        object_capability_t cap;
        bool                described;
} object_t;

static object_t objects[OBJECT_MAX];

/*
 * 物件侧的执行时钟：最近一次 object_run 的时刻。
 *
 * 它有两个用途：运动环排期（决定 16）与提交时的新鲜度判定（决定 10 修订）。
 * 基座不计时，所以时间只能存在物件侧。调用者必须给出单调递增的时刻。
 */
static u64 exec_now_ns;

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
        exec_now_ns = 0;
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
                objects[i].next_due_ns = 0;
                objects[i].step_count = 0;
                for (u8 k = 0; k < MOTION_DOF_MAX; k++)
                        objects[i].motion[k] = 0;
                zero(&objects[i].state, sizeof(objects[i].state));
                zero(&objects[i].cap, sizeof(objects[i].cap));
                objects[i].described = false;
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
         * 周期上限：积分用 64 位整数做 v * period_ns，若周期本身
         * 超过 2^63/2^32 秒（约 68 年），任何非零速度都会溢出。
         * 拒绝而不是截断 —— 申报一个跑不动的周期是物件侧的错。
         */
        if (period_ns > OBJECT_PERIOD_NS_MAX)
                return WIRE_NONE;

        for (u64 i = 0; i < OBJECT_MAX; i++) {
                if (objects[i].in_use)
                        continue;

                wire_t w = base_arrive();
                if (w == WIRE_NONE)
                        return WIRE_NONE;

                objects[i].in_use = true;
                objects[i].wire = w;
                objects[i].frame = FRAME_WORLD;
                objects[i].duration_ns = 0;
                objects[i].dof = 0;
                objects[i].event_count = 0;
                objects[i].unit = unit;
                objects[i].period_ns = period_ns;
                objects[i].carry[0] = 0;
                objects[i].carry[1] = 0;
                objects[i].carry[2] = 0;
                objects[i].next_due_ns = 0;
                objects[i].step_count = 0;
                for (u8 k = 0; k < MOTION_DOF_MAX; k++)
                        objects[i].motion[k] = 0;
                zero(&objects[i].state, sizeof(objects[i].state));
                zero(&objects[i].cap, sizeof(objects[i].cap));
                objects[i].described = false;

                return w;
        }
        return WIRE_NONE;
}

void object_unplug(wire_t w)
{
        object_t *o = find_by_wire(w);
        if (o == 0)
                return;

        base_depart(w);
        o->in_use = false;
        o->wire = WIRE_NONE;
}

/* ------------------------------------------------------------------ */
/* 基座的调用点                                                        */
/* ------------------------------------------------------------------ */

/*
 * 基座把已经校验过的事件递到这里。物件接住它，并自己负责后续 ——
 * 包括接之后继续沿轨道运动。
 *
 * 新鲜度判定（设计决定 10 修订）在这里做，而不是在基座里：
 * 事件只在 [issued_at_ns, issued_at_ns + duration_ns] 窗口内可被接，
 * 判定需要一个「现在」，而基座不计时。物件层的时间源就是执行时钟。
 */
motion_status_t object_submit(wire_t w, const motion_event_t *ev)
{
        object_t *o = find_by_wire(w);
        if (o == 0)
                return MOTION_E_NO_OBJECT;

        if (ev->duration_ns == 0)
                return MOTION_E_INVAL;            /* 零长度窗口永远无效 */

        u64 deadline = ev->issued_at_ns + ev->duration_ns;
        if (deadline < ev->issued_at_ns)
                return MOTION_E_INVAL;            /* 窗口端点回绕 */

        if (ev->issued_at_ns > exec_now_ns)
                return MOTION_E_INVAL;            /* 未来事件：时间来源异常 */

        if (exec_now_ns > deadline)
                return MOTION_E_DEADLINE;         /* 过期：拒绝陈旧命令 */

        o->dof = ev->dof;
        o->frame = ev->frame;
        o->duration_ns = ev->duration_ns;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                o->motion[i] = ev->motion[i];
        o->event_count++;

        return MOTION_OK;
}

/* ------------------------------------------------------------------ */
/* 自检用的只读查询                                                    */
/* ------------------------------------------------------------------ */

bool object_is_still(wire_t w)
{
        object_t *o = find_by_wire(w);
        if (o == 0)
                return false;

        for (u8 i = 0; i < o->dof; i++)
                if (o->motion[i] != 0)
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
        return o->motion[i];
}

/* ------------------------------------------------------------------ */
/* R8：物件自述与自持                                                  */
/*                                                                    */
/* 全部逐字段拷贝，不做整结构赋值 —— 大结构赋值可能让编译器调用        */
/* memcpy，而 freestanding 下没有链接 libc。                          */
/* ------------------------------------------------------------------ */

void object_describe(wire_t w, const object_capability_t *cap)
{
        object_t *o = find_by_wire(w);
        if (o == 0 || cap == 0)
                return;

        o->cap.dof = cap->dof;
        o->cap.mass = cap->mass;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++) {
                o->cap.limit_min[i] = cap->limit_min[i];
                o->cap.limit_max[i] = cap->limit_max[i];
                o->cap.max_rate[i]  = cap->max_rate[i];
        }
        o->described = true;
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

void object_set_state(wire_t w, const object_state_t *st)
{
        object_t *o = find_by_wire(w);
        if (o == 0 || st == 0)
                return;

        for (u8 i = 0; i < 3; i++) {
                o->state.position[i] = st->position[i];
                o->state.linear[i]   = st->linear[i];
                o->state.angular[i]  = st->angular[i];
        }
        for (u8 i = 0; i < 4; i++)
                o->state.orientation[i] = st->orientation[i];
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
static motion_fixed_t shr_round_32(__int128 v)
{
        const unsigned __int128 half = (unsigned __int128)1 << 31;

        if (v >= 0)
                return (motion_fixed_t)((v + (__int128)half) >> 32);

        /* |v| = -(v + 1) + 1，这种写法对 INT128_MIN 也成立 */
        unsigned __int128 mag = (unsigned __int128)(-(v + 1)) + 1u;
        return (motion_fixed_t)(-((__int128)((mag + half) >> 32)));
}

/* Q32.32 乘法：128 位中间积，避免溢出；不涉及除法，不需要编译器运行库。 */
static motion_fixed_t qmul(motion_fixed_t a, motion_fixed_t b)
{
        return shr_round_32((__int128)a * b);
}

/*
 * 用单位四元数 q = {w, x, y, z} 旋转三维向量 v：
 *     t  = 2 (u × v)
 *     v' = v + w t + u × t        其中 u = (x, y, z)
 *
 * 全零四元数视为「未申报姿态」，按无旋转处理。
 */
static void rotate3(const motion_fixed_t q[4], motion_fixed_t v[3])
{
        if (q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 0)
                return;

        motion_fixed_t w = q[0], x = q[1], y = q[2], z = q[3];
        motion_fixed_t t0 = 2 * (qmul(y, v[2]) - qmul(z, v[1]));
        motion_fixed_t t1 = 2 * (qmul(z, v[0]) - qmul(x, v[2]));
        motion_fixed_t t2 = 2 * (qmul(x, v[1]) - qmul(y, v[0]));

        v[0] += qmul(w, t0) + qmul(y, t2) - qmul(z, t1);
        v[1] += qmul(w, t1) + qmul(z, t0) - qmul(x, t2);
        v[2] += qmul(w, t2) + qmul(x, t1) - qmul(y, t0);
}

bool object_resolve(wire_t w, motion_fixed_t out[MOTION_DOF_MAX])
{
        object_t *o = find_by_wire(w);
        if (o == 0 || out == 0)
                return false;

        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                out[i] = o->motion[i];

        wire_t frame = o->frame;
        for (u32 depth = 0; frame != FRAME_WORLD; depth++) {
                if (depth >= OBJECT_FRAME_DEPTH_MAX)
                        return false;           /* 成环或链过深 */

                object_t *f = find_by_wire(frame);
                if (f == 0)
                        return false;           /* 参照系已离开 */

                /*
                 * out 是在参照系 f 的局部坐标下表达的运动：先按 f 的姿态
                 * 转到 f 的上一级坐标，再叠加 f 自身的运动。
                 * 线运动（0..2）与角运动（3..5）都是三维向量，各转一次。
                 */
                rotate3(f->state.orientation, &out[0]);
                rotate3(f->state.orientation, &out[3]);

                for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                        out[i] += f->motion[i];
                frame = f->frame;
        }
        return true;
}

/* ------------------------------------------------------------------ */
/* 执行模型（设计决定 16）：按物件自述的 period_ns 推进运动环           */
/*                                                                    */
/* 这一层在基座之外。基座不调度、不计时；时间由调用者（架构层）给出。 */
/* ------------------------------------------------------------------ */

/*
 * 物件的一步运动环：把解算出的相对世界速度积分进位置。
 *
 * motion 是速度（设计决定 18）：一步积分「速度 × 自述周期」，
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
 * 同样 9 ms，1 ms 周期与 3 ms 周期会差出 9 个最低位")，且周期越细
 * 误差越大——那不是「精度问题」，而是速度本身被周期改写了。
 * 余数进位把丢掉的零头存下来在下一次补上，误差只在 ±1 个最低位之间
 * 摆动而不累积，因此两条不同周期的积分路径给出逐比特相同的结果。
 *
 * 只积分线分量（0..2）到位姿；角分量（3..5）可被解算与直读，
 * 参考实现不积分姿态（那是物件自己的闭环的事）。
 * 参照链断开或成环时位置保持不动，但这一步仍算执行过。
 */
static void object_step(object_t *o)
{
        motion_fixed_t r[MOTION_DOF_MAX];

        if (object_resolve(o->wire, r)) {
                for (u8 i = 0; i < 3 && i < o->dof; i++) {
                        /*
                         * 余数进位积分（整数域，不碰 128 位除法）：
                         *
                         *   num  = v * period_ns + carry   （单位：LSB·ns）
                         *   step = num / 1e9               （本步整数位）
                         *   carry = num % 1e9              （留到下一步）
                         *
                         * 1e9 是编译期常量，除法被折成立即数与乘法；
                         * 128 位一般除法会拉进 __divti3，必须避开。
                         *
                         * 只能用 64 位中间量，因此要求
                         *     |v| * period_ns < 2^63 - 1e9
                         * 对 period_ns ≤ 10 ms 意味着 |v| ≤ 9.2e11 m/s，
                         * 远超任何运动系统的速度上限（见 object_plug 校验）。
                         *
                         * C 的 / 与 % 向零截断：负速度得到负步长负余数，
                         * 进位方向与运动方向一致。
                         */
                        /*
                         * 余数进位积分（只在 64 位整数域做除法）：
                         *
                         *   num  = v * period_ns + carry   （单位：LSB·ns）
                         *   step = num / 1e9               （本步整数位）
                         *   carry = num % 1e9              （留到下一步）
                         *
                         * carry 恒在 (−1e9, 1e9) 内，因此只要
                         * |v| * period_ns < 2^63 - 1e9，num 就装得进 int64，
                         * 除法不会退化成 128 位（128 位一般除法会拉进
                         * __divti3，裸机没有库可以链接）。object_plug()
                         * 校验 period_ns 上限，速度上限由调用者遵守。
                         *
                         * C 的 / 与 % 向零截断：负速度得到负步长负余数，
                         * 进位方向与运动方向一致。
                         */
                        motion_fixed_t num = r[i] * (motion_fixed_t)o->period_ns
                                           + o->carry[i];

                        o->state.position[i] += num / 1000000000LL;
                        o->carry[i] = num % 1000000000LL;
                }
        }
        o->step_count++;
}

u32 object_run(u64 now_ns)
{
        exec_now_ns = now_ns;   /* 同时是提交新鲜度的判定时钟 */
        u32 stepped = 0;

        for (u64 i = 0; i < OBJECT_MAX; i++) {
                object_t *o = &objects[i];

                if (!o->in_use || o->period_ns == 0)
                        continue;               /* 未申报周期：不推进 */
                if (o->next_due_ns != 0 && now_ns < o->next_due_ns)
                        continue;               /* 还没到期 */

                object_step(o);
                stepped++;

                if (o->next_due_ns == 0) {
                        o->next_due_ns = now_ns + o->period_ns;
                } else {
                        o->next_due_ns += o->period_ns;
                        /* 落后太多就不补跑，直接从现在重新排 */
                        if (o->next_due_ns <= now_ns)
                                o->next_due_ns = now_ns + o->period_ns;
                }
        }
        return stepped;
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