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
        u64            duration_ns;
        u8             dof;
        u32            event_count;
        u64            period_ns;     /* 物件自述的控制周期，由执行层兑现 */
        u64            next_due_ns;   /* 下一次运动环到期时刻；0 = 尚未起步 */
        u32            step_count;

        /* --- R8：物件自述与自持 --- */
        object_state_t      state;
        object_capability_t cap;
        bool                described;
} object_t;

static object_t objects[OBJECT_MAX];

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
        for (u64 i = 0; i < OBJECT_MAX; i++) {
                objects[i].in_use = false;
                objects[i].wire = WIRE_NONE;
                objects[i].frame = FRAME_WORLD;
                objects[i].duration_ns = 0;
                objects[i].dof = 0;
                objects[i].event_count = 0;
                objects[i].period_ns = 0;
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
        for (u64 i = 0; i < OBJECT_MAX; i++) {
                if (objects[i].in_use)
                        continue;

                wire_desc_t d;
                d.id = WIRE_NONE;
                d.unit = unit;
                d.period_ns = period_ns;
                d.online = true;

                wire_t w = base_arrive(&d);
                if (w == WIRE_NONE)
                        return WIRE_NONE;

                objects[i].in_use = true;
                objects[i].wire = w;
                objects[i].frame = FRAME_WORLD;
                objects[i].duration_ns = 0;
                objects[i].dof = 0;
                objects[i].event_count = 0;
                objects[i].period_ns = period_ns;
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
 * 包括事件到期之后继续沿轨道运动。
 */
motion_status_t object_submit(wire_t w, const motion_event_t *ev)
{
        object_t *o = find_by_wire(w);
        if (o == 0)
                return MOTION_E_NO_OBJECT;

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

/* Q32.32 乘法：128 位中间积，避免溢出；不涉及除法，不需要编译器运行库 */
static motion_fixed_t qmul(motion_fixed_t a, motion_fixed_t b)
{
        return (motion_fixed_t)(((__int128)a * b) >> 32);
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
 * 物件的一步运动环：把解算出的相对世界运动积分进位置。
 * 参考实现中 motion 的单位是「每个控制周期的位移」。
 * 参照链断开或成环时位置保持不动，但这一步仍算执行过。
 */
static void object_step(object_t *o)
{
        motion_fixed_t r[MOTION_DOF_MAX];

        if (object_resolve(o->wire, r)) {
                for (u8 i = 0; i < 3 && i < o->dof; i++)
                        o->state.position[i] += r[i];
        }
        o->step_count++;
}

u32 object_run(u64 now_ns)
{
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