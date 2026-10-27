/*
 * base.c — 基座实现
 *
 * 基座是一个无状态的运动控制函数。它持有的唯一东西是「接线」：
 * 哪个物件挂在哪个口。除此之外，它不记得任何东西。
 *
 * 注意本文件里没有什么：
 *   - 没有物件状态表
 *   - 没有「静止/运动」判定
 *   - 没有历史、日志、统计
 *   - 没有把事件留下来等以后用
 *
 * 一旦这里出现了「记住上一次事件」之类的字段，就是设计违规。
 */
#include "base.h"

/* ------------------------------------------------------------------ */
/* 接线表                                                              */
/*                                                                     */
/* 这是基座唯一的存储。它记录「谁挂在哪个口」，不记录「它在哪、它在   */
/* 做什么」。前者是执行的必要条件，后者是信息，基座不持有。           */
/* ------------------------------------------------------------------ */

/*
 * 表的大小是一个待定项（docs/design.md §9）。
 * 当前取 64：够验证模型，也小到能看出规模问题。
 */
#define WIRE_MAX 64

typedef struct {
        wire_desc_t desc;
        bool        in_use;
} wire_slot_t;

static wire_slot_t wire_table[WIRE_MAX];

/*
 * 基座初始化：把接线表清空。
 *
 * 为什么不依赖 .bss 的零初值：裸机引导器不一定清零 .bss，
 * 而接线表的状态直接决定「这条口有没有物件」。
 * 显式清一次，比信任加载器的行为可靠。
 */
void base_init(void)
{
        for (u64 i = 0; i < WIRE_MAX; i++) {
                wire_table[i].in_use = false;
                wire_table[i].desc.id = WIRE_NONE;
                wire_table[i].desc.unit = 0;
                wire_table[i].desc.period_ns = 0;
                wire_table[i].desc.online = false;
        }
}

/*
 * 物件侧的执行入口。
 *
 * 基座不认识物件内部是什么。它只把事件递过去，由物件自己的运动能力
 * 去执行。这个函数由各架构的物件接入层提供。
 */
extern motion_status_t object_submit(wire_t w, const motion_event_t *ev);

/* ------------------------------------------------------------------ */
/* 接线                                                                */
/* ------------------------------------------------------------------ */

static wire_slot_t *slot_of(wire_t w)
{
        if (w == WIRE_NONE || w > WIRE_MAX)
                return 0;
        /* 句柄从 1 开始，便于把 0 当作「无」 */
        return &wire_table[w - 1];
}

wire_t base_attach(const wire_desc_t *desc)
{
        if (desc == 0)
                return WIRE_NONE;

        for (u64 i = 0; i < WIRE_MAX; i++) {
                if (!wire_table[i].in_use) {
                        wire_table[i].desc = *desc;
                        wire_table[i].in_use = true;
                        wire_table[i].desc.id = (wire_t)(i + 1);
                        wire_table[i].desc.online = true;
                        return wire_table[i].desc.id;
                }
        }

        /* 接线表满 */
        return WIRE_NONE;
}

void base_detach(wire_t w)
{
        wire_slot_t *s = slot_of(w);
        if (s == 0 || !s->in_use)
                return;

        /*
         * 只把接线标记为不在用。不保存任何「它曾经在哪」的信息 ——
         * 物件离开后，基座对它就一无所知，这是设计要求。
         */
        s->in_use = false;
        s->desc.id = WIRE_NONE;
        s->desc.online = false;
        s->desc.period_ns = 0;
        s->desc.unit = 0;
}

bool base_wire_valid(wire_t w)
{
        wire_slot_t *s = slot_of(w);
        return s != 0 && s->in_use && s->desc.online;
}

/* ------------------------------------------------------------------ */
/* 原语                                                                */
/* ------------------------------------------------------------------ */

motion_status_t base_apply(const motion_event_t *ev)
{
        if (ev == 0)
                return MOTION_E_INVAL;

        if (ev->dof > MOTION_DOF_MAX)
                return MOTION_E_INVAL;

        /* 无线填为 0 是有效的「无运动分量」，但目标必须存在 */
        if (!base_wire_valid(ev->wire))
                return MOTION_E_NO_WIRE;

        /*
         * 参照系：0 表示世界系，始终有效。
         * 否则它也是一条接线，必须当前存在。
         */
        if (ev->frame != FRAME_WORLD && !base_wire_valid(ev->frame))
                return MOTION_E_NO_FRAME;

        /*
         * 把事件交给物件。基座到此为止 —— 不缓存、不排队、不计时。
         * 事件到期后基座不做任何事：物件按该事件继续运动。
         */
        return object_submit(ev->wire, ev);
}