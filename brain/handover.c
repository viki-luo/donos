/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * handover.c — 换命令来源，不搬状态
 */
#include "handover.h"

const char *control_kind_name(control_kind_t kind)
{
        switch (kind) {
        case CONTROL_NONE:     return "none";
        case CONTROL_SPEED:    return "speed-loop";
        case CONTROL_POSITION: return "position-loop";
        }

        return "unknown";
}

bool control_install(control_source_t *cs, control_kind_t kind, wire_t w,
                     u64 period_ns, motion_fixed_t target,
                     motion_fixed_t gain, motion_fixed_t limit)
{
        control_loop_t fresh;

        if (cs == 0 || kind == CONTROL_NONE)
                return false;

        /*
         * 先把新控制律在**局部**变量里初始化好，成功了才覆盖。
         * 先清空再初始化会让一次失败的安装把旧控制器也弄丢。
         */
        bool ok = false;

        if (kind == CONTROL_SPEED)
                ok = wheel_control_init(&fresh, w, period_ns, target, gain, limit);
        else if (kind == CONTROL_POSITION)
                ok = joint_control_init(&fresh, w, period_ns, target, gain, limit);

        if (!ok)
                return false;

        cs->kind = kind;
        cs->loop = fresh;
        cs->installs++;

        /*
         * 刻意不读旧 loop 的任何字段：这里是「换来源」，
         * 不是「交接」。物件自己的实测就是新控制器唯一的初始信息。
         */
        return true;
}

void control_uninstall(control_source_t *cs)
{
        if (cs == 0)
                return;

        cs->kind = CONTROL_NONE;
        cs->loop.commands_sent = 0;
        cs->loop.refusals = 0;
        cs->loop.last_run_ns = 0;
}

bool control_step(control_source_t *cs, u64 now_ns)
{
        if (cs == 0)
                return false;

        switch (cs->kind) {
        case CONTROL_SPEED:
                return wheel_control_step(&cs->loop, now_ns);
        case CONTROL_POSITION:
                return joint_control_step(&cs->loop, now_ns);
        case CONTROL_NONE:
                return false;
        }

        return false;
}
