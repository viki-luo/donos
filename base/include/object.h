/*
 * object.h — 物件侧接口（参考实现）
 *
 * 这一侧不属于基座。基座只知道「谁挂在哪个口」并把事件递过来；
 * 物件知道自己在哪、在做什么，并自己保存这些。
 *
 * 运动状态存在物件侧，而不是 base.c 里 —— 这条分界就是设计本身：
 * 一旦运动状态搬进基座，基座就有状态了，整个模型随之崩塌。
 *
 * 本头文件供自检与调试使用。真实物件通常由驱动实现同一组语义，
 * 不一定要链接这份参考实现。
 */
#ifndef LITEOS_OBJECT_H
#define LITEOS_OBJECT_H

#include "base.h"

#define OBJECT_MAX 64

/*
 * 物件侧初始化：清空物件表。
 * 理由同 base_init：不信任引导器会清 .bss。
 */
void object_init(void);

/*
 * 物件插入基座：物件先申请一条接线，基座登记「谁挂在哪个口」。
 * 成功返回接线句柄，失败返回 WIRE_NONE。
 */
wire_t object_plug(u32 unit, u64 period_ns);

/* 物件离开基座。此后该接线上的事件都会被拒绝。 */
void object_unplug(wire_t w);

/*
 * 基座把已经校验过的事件递到这里。物件接住它，并自己负责后续 ——
 * 包括事件到期之后继续沿轨道运动。
 */
motion_status_t object_submit(wire_t w, const motion_event_t *ev);

/* ---- 只读查询（自检与调试用）---- */

/* 该物件当前是否相对其参照系静止（所有有效分量为零）。 */
bool object_is_still(wire_t w);

/* 该物件累计收到过多少个事件。 */
u32 object_event_count(wire_t w);

#endif /* LITEOS_OBJECT_H */
