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