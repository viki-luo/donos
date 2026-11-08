/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * monitor.h — 基座之外的安全监视
 *
 * 为什么它不在基座里：
 *   基座只管「让物件动起来」这一件事，它不知道物件该不该动、
 *   动得对不对。一旦基座开始判断「这个命令危险」，它就拥有了
 *   对世界的意见 —— 而那正是「无状态、无判断」的终点。
 *
 * 所以监视器是基座之上的一个**观察者**：
 *   - 只读：base_wire_valid / object_state / object_capability /
 *     object_resolve / object_run 的返回值；
 *   - 不写：从不调用 base_apply，从不 object_set_state；
 *   - 无权限：它的结论是「事件」，交给上层大脑去处置。
 *
 * 这条「只观察不干预」由 monitor_verify() 的 before/after 比对证明：
 * 一次 poll 之后，所有物件的状态必须逐字段不变。
 */
#ifndef LITEOS_MONITOR_H
#define LITEOS_MONITOR_H

#include "object.h"

typedef enum {
        MON_OK = 0,
        MON_E_WIRE_LOST = -1,      /* 登记过的接线消失了 */
        MON_E_DRIFT = -2,          /* 实测速度与命令的偏差超出阈值 */
        MON_E_RESOLVE = -3,        /* 世界解算失败（断链/成环/溢出） */
        MON_E_RATE = -4,           /* 实测速度超出物件自报的上限 */
        MON_E_CLOCK = -5,          /* 执行层拒绝了本次时刻 */
} monitor_event_t;

/* 一次轮询的统计，供演示与自检读 */
typedef struct {
        u32 watched;        /* 登记在册的接线数 */
        u32 alive;          /* 其中仍然有效的 */
        u32 events;         /* 本次倾泻出的事件数 */
} monitor_stats_t;

/*
 * 监视器上报的出口。真实系统里它是日志、断言线或另一个大脑；
 * 自检里它是一段记录代码。监视器不认识出口的实现。
 */
typedef void (*monitor_sink_t)(monitor_event_t ev, wire_t w);

void monitor_init(monitor_sink_t sink);

/* 登记一条需要持续关注的接线（例如刚插上的物件）。 */
bool monitor_watch(wire_t w);

/* 放弃关注（物件正常离开时调用，这样它的消失不会被报成故障）。 */
void monitor_forget(wire_t w);

/*
 * 只观察现场并返回统计，不推进执行层、不修改物件状态。
 * run_status 是调用者单独推进执行层后得到的返回值。
 * 事件接收回调也必须遵守不修改物件状态的约定。
 */
monitor_stats_t monitor_poll(object_run_status_t run_status);

/* 「实测与命令的偏差」判定阈值（Q32.32）。 */
void monitor_set_drift_limit(motion_fixed_t limit);

/* 确定性自检。返回失败计数。 */
int monitor_verify(void);

#endif
