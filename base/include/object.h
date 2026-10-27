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

/* ------------------------------------------------------------------ */
/* 位姿、状态、能力                                                    */
/*                                                                     */
/* 全部在物件侧，基座一个字段都看不到。                                */
/*                                                                     */
/* 为什么这些绝不能进基座（设计文档 §3.7）：                          */
/*   1. 位姿是相对量 —— 存它就要存「相对谁」，即全局世界模型        */
/*   2. 位姿每周期都变 —— 存它就要有人持续更新，即调度               */
/*   3. 位姿是判定静止的依据 —— 基座能判定就篡了大脑的权             */
/*                                                                     */
/* 三条任意一条成立，基座就不再是无状态函数。                          */
/* ------------------------------------------------------------------ */

/*
 * 位姿 + 速度。全部定点（Q32.32），跨界不出现浮点。
 *
 * orientation 是单位四元数 {w, x, y, z}，用四元数而非欧拉角：
 * 没有万向锁，插值也稳定。
 */
typedef struct {
        motion_fixed_t position[3];      /* x, y, z */
        motion_fixed_t orientation[4];   /* w, x, y, z（单位四元数） */
        motion_fixed_t linear[3];        /* 线速度 */
        motion_fixed_t angular[3];       /* 角速度 */
} object_state_t;

/*
 * 能力：物件自述的「我能做什么」，基本不变。
 *
 * 这是 R8「可自述」的载体。它属于「能力」而非「状态」：
 * 能力回答「我能怎样」，状态回答「我现在怎样」。
 */
typedef struct {
        u8             dof;                        /* 有效自由度 */
        motion_fixed_t limit_min[MOTION_DOF_MAX];  /* 每维下限 */
        motion_fixed_t limit_max[MOTION_DOF_MAX];  /* 每维上限 */
        motion_fixed_t max_rate[MOTION_DOF_MAX];   /* 每维最大速率 */
        motion_fixed_t mass;                       /* 质量；0 表示未申报 */
} object_capability_t;

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

/* ---- 物件自述与自持（R8）：只对大脑可见，基座无路径 ---- */

/* 物件申报自己的能力。申报前 object_capability 返回 false。 */
void object_describe(wire_t w, const object_capability_t *cap);

/* 读取物件申报过的能力。未申报或接线无效返回 false。 */
bool object_capability(wire_t w, object_capability_t *out);

/* 物件更新自己的位姿与速度（由物件自己的控制环调用）。 */
void object_set_state(wire_t w, const object_state_t *st);

/* 读取物件当前的位姿与速度。接线无效返回 false。 */
bool object_state(wire_t w, object_state_t *out);

/* ---- 参照系由物件自己解（设计决定 15，方案 A）---- */

/* 参照链的最大深度。超过即视为成环。 */
#define OBJECT_FRAME_DEPTH_MAX 8

/*
 * 把物件「相对 frame 的运动」解成「相对世界的运动」。
 *
 * 沿参照链逐级直读参照系物件的运动 —— 这是物件间直连，不经过基座。
 * 基座不能做这件事：解 frame 要读另一个物件的运动，那是世界信息。
 *
 * 参照系已离开、链断开或成环时返回 false：基座只校验 frame 这条线
 * 当下是否有效，发现不了环，环只能由物件侧发现。
 *
 * 参考实现只做逐分量相加（平移合成），不按参照系姿态旋转。
 */
bool object_resolve(wire_t w, motion_fixed_t out[MOTION_DOF_MAX]);

/* ---- 执行模型（设计决定 16）：在基座之外推进各物件的运动环 ---- */

/*
 * 推进所有到期物件的运动环一步，返回本次推进的物件数。
 *
 * now_ns 由调用者（架构层的时钟）给出：基座不持有时间，执行层也不自己
 * 读硬件。每个物件按自己在到达时申报的 period_ns 被推进；period_ns 为 0
 * 的物件不会被推进。落后时不补跑，从当前时刻重新排期。
 */
u32 object_run(u64 now_ns);

/* 该物件的运动环累计被推进了多少步。 */
u32 object_step_count(wire_t w);

/* ---- 只读查询（自检与调试用）---- */

/* 该物件当前是否相对其参照系静止（所有有效分量为零）。 */
bool object_is_still(wire_t w);

/* 该物件累计收到过多少个事件。 */
u32 object_event_count(wire_t w);

/*
 * 读取该物件第 i 个自由度上的运动量。
 * 越界或接线无效时返回 0。
 *
 * 这是「物件即记忆」的接口：大脑换上来之后，就是靠它直接读现场，
 * 不需要经过基座。
 */
motion_fixed_t object_motion(wire_t w, u8 i);

#endif /* LITEOS_OBJECT_H */
