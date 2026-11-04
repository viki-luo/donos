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
 * 物件可申报的控制周期上限：1 秒。
 *
 * 这是执行模型的上界，不是算术安全证明。积分由 checked fixed-point
 * 层完成：它先分解速度，再分别计算整秒项与不足 1e9 的余数项，避免
 * 有符号乘法溢出和 128 位除法运行时依赖。1 秒仍远高于常见运动控制环
 * （100 Hz ~ 1 kHz），同时给调度与边界测试一个明确上界。
 */
#define OBJECT_PERIOD_NS_MAX 1000000000ULL

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
 * 速度的量纲随通道冻结（设计决定 18）：
 *   linear  单位 m/s；angular 单位 rad/s。
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
 * 基座把已经校验过的事件递到这里。物件接住它前先做新鲜度判定
 * （设计决定 10 修订）：事件只在 [issued_at_ns, issued_at_ns + duration_ns]
 * 窗口内可被接受，判定时刻 now_ns 是该事件本次到达入口的真实提交时刻，
 * 不得复用上一次 object_run 的旧节拍。过期返回 MOTION_E_DEADLINE，
 * 未来事件与零窗口返回 MOTION_E_INVAL。接受之后物件持续沿该事件运动。
 */
motion_status_t object_submit(wire_t w, u64 now_ns,
                              const motion_event_t *ev);

/* ---- 物件自述与自持（R8）：只对大脑可见，基座无路径 ---- */

/*
 * 物件申报自己的能力。成功返回 true。
 *
 * 申报会被**校验**，不合法的申报整体拒绝、不留下半份记录。拒绝条件：
 *   - dof > MOTION_DOF_MAX
 *   - 有效轴（i < dof）上 limit_min[i] > limit_max[i]（区间反向）
 *   - 有效轴（i < dof）上 max_rate[i] < 0（速率为负没有意义）
 *   - mass < 0
 * dof 之外的轴一律按「未申报」清零，与事件的 dof 尾部同一条纪律：
 * 声明之外的字段不进入模型，也就不会被后来的读者捡到。
 *
 * 校验失败返回 false 且 object_capability 继续返回 false —— 一份被
 * 悄悄打折的申报比没有申报更危险，而静默接受非法申报正是死字段的来源。
 */
bool object_describe(wire_t w, const object_capability_t *cap);

/*
 * 读取物件申报过的能力。未申报、申报被拒或接线无效返回 false。
 */
bool object_capability(wire_t w, object_capability_t *out);

/*
 * 物件更新自己的位姿与速度（由物件自己的控制环调用）。
 *
 * state 里的 position/linear/angular 全部在**本物件自己的参照系**里表达，
 * 不是世界坐标；世界视角由 object_resolve() 给出。
 *
 * 调用会清空积分余数（carry）：余数属于被改写前的那条轨迹，留着它
 * 会让新轨迹凭空多出一个取决于历史的零头。
 */
void object_set_state(wire_t w, const object_state_t *st);

/*
 * 读取物件当前的位姿与速度（参照系内的实测值）。接线无效返回 false。
 */
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
 * 逐级复合的是完整的刚体速度（twist），不是简单的向量相加：
 *   1. 按参照系姿态把本级的线速度、角速度、相对位置一起转到上一级坐标；
 *   2. 位置耦合项 ω_F × r 计入线速度 —— 挂在自转参照系上的物件会被
 *      「转出去」，少了这一项它只会跟着平移；
 *   3. 叠加参照系自身的线速度与角速度；
 *   4. 本级原点相对上一级原点的位移也一起上移，否则上一级会绕错原点转。
 * 其中 r 取**实测**位置（state.position），不取命令 —— 耦合项取决于物件
 * 实际待在哪里。未申报姿态（全零四元数）视为无旋转。
 *
 * out[0..2] 是世界线速度（m/s），out[3..5] 是世界角速度（rad/s）。
 */
bool object_resolve(wire_t w, motion_fixed_t out[MOTION_DOF_MAX]);

/* ---- 执行模型（设计决定 16）：在基座之外推进各物件的运动环 ---- */

typedef enum {
        OBJECT_RUN_OK = 0,
        OBJECT_RUN_E_CLOCK_ROLLBACK = -1,
        OBJECT_RUN_E_TIME_OVERFLOW = -2,
        OBJECT_RUN_E_INVAL = -3,
} object_run_status_t;

/*
 * 推进所有到期物件的运动环一步，并把本次推进数写到 stepped。
 *
 * now_ns 由调用者（架构层时钟）给出。执行层拒绝时间回退，并对所有
 * now_ns + period_ns 调度加法做回绕检查。物件第一次被执行层看见时只
 * 建立 next_due_ns，不积分一个尚未经过的完整周期。落后时不补跑，
 * 而是从当前时刻重新排期。提交新鲜度使用 base_apply 的显式 now_ns，
 * 与此调度时钟互不混用。
 */
object_run_status_t object_run(u64 now_ns, u32 *stepped);

/*
 * 执行层当前所在的时刻，即最近一次被接受的 object_run 的 now_ns。
 *
 * 存在的理由：调用者手里的硬件节拍计数与执行层的时间轴不是同一条。
 * 若把「节拍计数 × 每拍纳秒」直接喂给 object_run，两者一旦错位就是
 * 一次真实的时间回退，执行层会拒绝，物件从此不再前进。调用者应当以
 * 本值为基准加上自己测得的时间增量，从而始终单调前进。
 */
u64 object_now_ns(void);

/* 该物件的运动环累计被推进了多少步。 */
u32 object_step_count(wire_t w);

/*
 * 读取物件到达时自述的接线属性（R3）。接线无效返回 false。
 * 这是物件侧保存的那一份，供大脑直读，不经过基座。
 */
bool object_declared(wire_t w, u32 *unit, u64 *period_ns);

/* ---- 只读查询（自检与调试用）---- */

/*
 * 本物件**被告知**的运动是否全零（相对其参照系，所有有效分量为零）。
 *
 * 名字不叫 is_still 是有意的：它读的是命令，不是世界里的运动。
 * 「命令为零」只说「本物件不相对 frame 动」；参照系自己在动时，
 * 它照样在世界里走 —— 静止是一种相对的运动，而这句话的另一半是
 * 「相对谁」。世界视角的动与不动要用 object_resolve() 判断。
 */
bool object_commanded_is_zero(wire_t w);

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
