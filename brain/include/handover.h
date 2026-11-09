/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * handover.h — 控制律热插拔
 *
 * 主张（设计决定 28 的延伸）：大脑是**事件的来源**，不是一条状态的持有者。
 * 换控制律 = 换来源，不做交接、不搬历史、不改基座、不动物件状态。
 *
 * 因此本接口刻意**没有**「导出旧控制器状态」或「导入新控制器状态」这类
 * 函数。如果有，热插拔就变成了状态迁移，而状态迁移正是「基座无状态」
 * 想要避免的东西 —— 一旦允许迁移，就必然有人把物件的位姿也搬过去，
 * 于是大脑开始替基座保存信息。
 *
 * 新控制器接手的第一个周期只记录时刻、不发命令：它还没见过实测，
 * 此刻的任何误差都是凭空的。与执行层「首次只排期」是同一条纪律。
 */
#ifndef DONOS_HANDOVER_H
#define DONOS_HANDOVER_H

#include "control.h"

typedef enum {
        CONTROL_NONE = 0,       /* 没有控制器：物件按新鲜度窗口自然过期 */
        CONTROL_SPEED,          /* 速度环：控制线速度 */
        CONTROL_POSITION,       /* 位置环：控制关节角 */
} control_kind_t;

typedef struct {
        control_kind_t kind;
        control_loop_t loop;
        u32            installs;        /* 被换上来几次 */
} control_source_t;

/*
 * 把某个控制律装到这条接线上。
 *
 * 旧控制器的任何字段都不会被读走：只覆盖本结构，然后返回。
 * 接线无效、参数非法时返回 false，且**保留**原有的控制器（宁可不动，
 * 也不要让一条接线在失败后失去控制）。
 */
bool control_install(control_source_t *cs, control_kind_t kind, wire_t w,
                     u64 period_ns, motion_fixed_t target,
                     motion_fixed_t gain, motion_fixed_t limit);

/* 摘掉当前控制律（物件不再被命令，直到有新的来源）。 */
void control_uninstall(control_source_t *cs);

/*
 * 推进一步。返回 true 表示这一步确实发出了命令。
 * 没有控制器时返回 false 且不触碰基座。
 */
bool control_step(control_source_t *cs, u64 now_ns);

/* 当前控制律的名字，用于日志与断言（不参与控制）。 */
const char *control_kind_name(control_kind_t kind);

#endif /* DONOS_HANDOVER_H */
