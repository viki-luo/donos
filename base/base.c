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
        u32  generation;   /* 该槽位被用过的次数，防止旧句柄复活 */
        bool in_use;
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
                wire_table[i].generation = 0;
        }
}

/*
 * 物件侧的执行入口。
 *
 * 基座不认识物件内部是什么。它只把事件递过去，由物件自己的运动能力
 * 去执行。这个函数由各架构的物件接入层提供。
 */
extern motion_status_t object_submit(wire_t w, u64 now_ns,
                                     const motion_event_t *ev);

/* ------------------------------------------------------------------ */
/* 接线                                                                */
/* ------------------------------------------------------------------ */

/*
 * 由句柄找回槽位。
 *
 * 必须同时校验「槽位在用」和「世代号一致」：只校验下标的实现，
 * 会让离开的句柄在槽位被复用时重新变得有效 —— 那是事故源头。
 */
static wire_slot_t *slot_of(wire_t w)
{
        u32 idx = WIRE_INDEX(w);

        /* 高 16 位必须是 0：否则是同一句柄的非规范别名 */
        if ((w & ~WIRE_CANONICAL_MASK) != 0)
                return 0;

        if (idx == 0 || idx > WIRE_MAX)
                return 0;

        wire_slot_t *s = &wire_table[idx - 1];
        if (!s->in_use || s->generation != WIRE_GEN(w))
                return 0;

        return s;
}

wire_t base_arrive(void)
{
        for (u64 i = 0; i < WIRE_MAX; i++) {
                if (!wire_table[i].in_use) {
                        /* 世代号单调递增，0 保留给「无」 */
                        wire_table[i].generation++;
                        if (wire_table[i].generation == 0)
                                wire_table[i].generation = 1;

                        wire_table[i].in_use = true;

                        return WIRE_MAKE(wire_table[i].generation, (u32)(i + 1));
                }
        }

        /* 接线表满 */
        return WIRE_NONE;
}

void base_depart(wire_t w)
{
        wire_slot_t *s = slot_of(w);
        if (s == 0 || !s->in_use)
                return;

        /*
         * 只把接线标记为不在用。不保存任何「它曾经在哪」的信息 ——
         * 物件离开后，基座对它就一无所知，这是设计要求。
         */
        s->in_use = false;
}

bool base_wire_valid(wire_t w)
{
        return slot_of(w) != 0;
}

/* ------------------------------------------------------------------ */
/* 原语                                                                */
/* ------------------------------------------------------------------ */

motion_status_t base_apply(u64 now_ns, const motion_event_t *ev)
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
         * 把事件和本次提交时刻交给物件。基座到此为止：不缓存、不排队、
         * 不保存时钟。新鲜度检查只使用这次调用的 now_ns；事件一旦接受，
         * 窗口闭合也不会撤销物件已经自持的轨迹。
         */
        return object_submit(ev->wire, now_ns, ev);
}