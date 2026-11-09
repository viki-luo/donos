/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * brain.c — 上层大脑的参考实现（基座之外）
 *
 * 本文件属于「基座之上」，不是基座的一部分。它可以随便换、
 * 随便重写、随便插拔，而 base/ 一行都不用改 —— 这就是分层的意义。
 *
 * 刻意没有任何 malloc：大脑是静态分配的，数量在编译期确定。
 * 一个热插拔系统的部件不该依赖堆，否则「拔掉」之后泄漏就成了常态。
 */
#include "brain.h"
#include "object.h"

#define BRAIN_MAX 4

static brain_t brains[BRAIN_MAX];

/* ------------------------------------------------------------------ */
/* 决策律                                                              */
/* ------------------------------------------------------------------ */

/*
 * cruise：恒定沿世界 x 轴 1 m/s。
 *
 * 窗口取 4 个决策周期：大脑每周期都会重新下令，窗口只需要覆盖
 * 「两次决策之间的正常抖动」。取无穷大等于放弃新鲜度，
 * 那样旧大脑下过的命令会在它离开后永远有效。
 */
static void decide_cruise(brain_t *self, u64 now_ns)
{
        motion_event_t ev;
        ev.wire = self->target;
        ev.frame = FRAME_WORLD;
        ev.issued_at_ns = now_ns;
        ev.duration_ns = 4 * self->period_ns;
        ev.dof = 1;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = 0;
        ev.motion[0] = (motion_fixed_t)1 << 32;          /* 1 m/s */

        base_apply(now_ns, &ev);
}

/*
 * shuttle：读到目标相对自己参照系的位置，往原点方向推。
 *
 * 注意这个函数**只**读 object_state() —— 它不知道上一个大脑是谁、
 * 做过什么、下过多大的命令。它看到的是物件自持的实测位置。
 * 这就是「物件即记忆」：换大脑不需要交接，因为记忆不在大脑那里。
 */
static void decide_shuttle(brain_t *self, u64 now_ns)
{
        object_state_t st;
        motion_event_t ev;

        if (!object_state(self->target, &st))
                return;                                 /* 目标不在了 */

        ev.wire = self->target;
        ev.frame = FRAME_WORLD;
        ev.issued_at_ns = now_ns;
        ev.duration_ns = 4 * self->period_ns;
        ev.dof = 1;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = 0;

        /* 在正侧就反向推，在负侧就正向推 */
        ev.motion[0] = st.position[0] >= 0 ? -((motion_fixed_t)1 << 31)
                                           : ((motion_fixed_t)1 << 31);

        base_apply(now_ns, &ev);
}

/* ------------------------------------------------------------------ */
/* 装配                                                                */
/* ------------------------------------------------------------------ */

static brain_t *attach(const char *name, wire_t target, u64 period_ns,
                       void (*decide)(brain_t *, u64))
{
        if (target == WIRE_NONE || period_ns == 0)
                return 0;

        for (u32 i = 0; i < BRAIN_MAX; i++) {
                if (brains[i].name != 0)
                        continue;

                /*
                 * 大脑自己的接线也是一条普通接线：走同一个 object_plug，
                 * 拿同一个编码的句柄，没有任何保留位。
                 */
                wire_t w = object_plug(2, period_ns);
                if (w == WIRE_NONE)
                        return 0;

                brains[i].name = name;
                brains[i].wire = w;
                brains[i].target = target;
                brains[i].period_ns = period_ns;
                brains[i].decisions = 0;
                brains[i].decide = decide;
                return &brains[i];
        }
        return 0;
}

brain_t *brain_cruise(wire_t target, u64 period_ns)
{
        return attach("cruise", target, period_ns, decide_cruise);
}

brain_t *brain_shuttle(wire_t target, u64 period_ns)
{
        return attach("shuttle", target, period_ns, decide_shuttle);
}

motion_status_t brain_tick(brain_t *b, u64 now_ns)
{
        if (b == 0 || b->decide == 0)
                return MOTION_E_INVAL;

        b->decide(b, now_ns);
        b->decisions++;
        return MOTION_OK;
}

void brain_detach(brain_t *b)
{
        if (b == 0)
                return;

        /*
         * 只交还大脑自己的接线。目标物件一根手指都不碰 ——
         * 它继续沿最后被接受的命令运动。若在这里顺手把目标也停了，
         * 「换大脑」就变成了「换大脑时机器停一下」，而那是事故。
         */
        object_unplug(b->wire);
        b->name = 0;
        b->wire = WIRE_NONE;
        b->target = WIRE_NONE;
        b->decisions = 0;
}
