/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * brain.h — 上层大脑（基座之外）
 *
 * 基座没有「大脑槽」。这句话不是修辞，而是可被静态断言的事实：
 * base/base.c 里不允许出现 brain 这个词，make check-purity 会失败。
 *
 * 那么「大脑热插拔」到底是什么？就是**事件来源的替换**：
 *   旧大脑停止调用 base_apply，新大脑开始调用。
 * 基座全程不知情，也不需要知情 —— 它只认接线。
 *
 * 由此得到一条比「热插拔」更强的性质：**换大脑时没有交接物**。
 * 新大脑读的是物件自持的现场（object_state / object_resolve），
 * 而不是旧大脑留下的变量。物件即记忆，记忆不在大脑手上。
 */
#ifndef LITEOS_BRAIN_H
#define LITEOS_BRAIN_H

#include "base.h"

typedef struct brain brain_t;

struct brain {
        const char *name;
        wire_t      wire;        /* 大脑自己的接线：普通接线，无特权 */
        wire_t      target;      /* 它驱动的那只物件 */
        u64         period_ns;   /* 决策周期 */
        u32         decisions;   /* 累计决策次数 */
        /*
         * 决策：只接收「目标」与「现在」。
         * 刻意不接收任何 from_previous_brain 之类的参数 —— 一旦需要，
         * 就说明现场被搬到了大脑手里，架构已经走样。
         */
        void      (*decide)(brain_t *self, u64 now_ns);
};

/*
 * 两个控制律不同的大脑。
 *   cruise  —— 沿世界 x 轴匀速前进
 *   shuttle —— 读到目标在原点哪一侧，就往回推
 * 它们的判断依据分别是「常量」和「现场」，所以 swap 之后运动的
 * 方向变化是**由现场决定**的，不是被硬编码的。
 */
brain_t *brain_cruise(wire_t target, u64 period_ns);
brain_t *brain_shuttle(wire_t target, u64 period_ns);

/* 一次决策：读现场 → base_apply。返回提交状态。 */
motion_status_t brain_tick(brain_t *b, u64 now_ns);

/*
 * 大脑离开：交还它自己的接线。
 * 目标物件不受影响，继续沿最后被接受的命令运动（决定 10 修订：
 * 接受即自持）。这正是「换大脑时机器不会停」的原因。
 */
void brain_detach(brain_t *b);

/* 确定性自检：切换前、切换中、切换后的语义。返回失败计数。 */
int brain_verify(void);

#endif
