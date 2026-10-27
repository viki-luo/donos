# 变更记录 / Changelog

本项目所有重要变更都将记录于此。
所有变更遵循 [Keep a Changelog](https://keepachangelog.com/) 格式，
版本号遵循 [语义化版本 / Semantic Versioning](https://semver.org/)。

*All notable changes to this project are documented here, following [Keep a Changelog](https://keepachangelog.com/) and [Semantic Versioning](https://semver.org/).*

---

## [Unreleased] / 未发布

### Added / 新增

- 完整设计文档（中英双语）/ Full design document (bilingual)
- 贡献指南与行为准则 / Contributing guide and code of conduct
- Issue 与 PR 模板 / Issue and PR templates
- 引导路径文档（中英双语）/ Boot path documentation (bilingual),
  covering the PVH 32-bit entry, the 32→64 transition, and the
  file-backed writable segment requirement
- 基座实现：`base_init` / `base_apply` / `base_arrive` / `base_depart`，
  以及物件侧的 `object_init` / `object_plug` / `object_unplug` /
  `object_submit` / `object_is_still`
  / Base implementation and the object-side counterpart
- 启动自检：11 项断言覆盖运动语义、无效接线、无效参照系与越界自由度
  / Boot self-test: 11 assertions covering motion semantics, invalid
  wires, invalid frames, and out-of-range degrees of freedom
- 公共短类型头 `base/include/types.h`，让架构层不必依赖 `base.h`
  / A shared typedef header so the architecture layer need not depend on `base.h`
- 架构层：串口控制台、端口 I/O、IDT、8259 PIC 重映射、PIT 周期中断与开中断
  / Architecture layer: serial console, port I/O, IDT, 8259 PIC remap, PIT
  periodic interrupt and enabling interrupts
- **异常处理**：CPU 异常不再变成三重故障后静默停住，而是打印向量号、
  错误码与出错指令地址（`make FAULT_TEST=1` 可验证）
  / **Exception handling**: a CPU exception now prints its vector, error code
  and faulting instruction address instead of silently triple-faulting
  (`make FAULT_TEST=1` verifies this)
- **总线发现**：PCI 扫描找到真实设备，接线由此自动产生。此前接线靠测试
  代码手写 `object_plug` 造出来；现在来自硬件扫描。发现的 6 个设备
  （IDE、VGA、e1000 等）在 QEMU 上验证通过
  / **Bus discovery**: a PCI scan finds real devices and wiring arises from it.
  Earlier wiring was created by hand-written `object_plug` calls in tests; now
  it comes from scanning hardware. Six devices (IDE, VGA, e1000, ...) verified on QEMU

- **统一模型验证**（`base/verify.c`）：用同一套原语建一个「大脑」和两个
  「手脚」，验证基座无法区分它们 —— 大脑没有特权槽、换大脑与物件热插拔
  同机制、同一接线既是大脑又是可被驱动的物件
  / **Unified-model verification** (`base/verify.c`): a brain and two limbs built
  with the same primitive, showing the base cannot tell them apart — no
  privileged slot, brain swap uses the same mechanism as object hot-plug, and
  the same wire is both a brain and a drivable object
- `object_motion()`：读取物件某个自由度的运动量，供「大脑直接读现场」使用
  / `object_motion()`: read one degree of freedom of an object, so a newly
  attached brain can read the situation directly
- **双向接口契约**（设计文档 §3.6–3.8）：集中写清跨越基座边界的全部数据 ——
  发给物件的只有 `motion_event_t`，从物件只收 `unit` / `period_ns`，
  没有查询、没有状态上报；并说明「零个状态」为何是设计而非缺失
  / **Bidirectional interface contract** (design §3.6–3.8): all data crossing the
  base boundary in one place — only `motion_event_t` is sent to an object, only
  `unit` / `period_ns` are received from it, with no query and no state report;
  and why "zero states" is design rather than omission
- **物件符合性标准**（`docs/conformance.md`，中英双语）：R1–R8 八条要求及各自的
  验证状态；能力与状态的区分；3D 位姿为何不进基座
  / **Object conformance standard** (`docs/conformance.md`, bilingual): requirements
  R1–R8 with their verification status, the capability/state split, and why 3D
  pose stays out of the base
- **R8 可自述**：物件侧新增 `object_state_t`（位置、姿态四元数、线/角速度）与
  `object_capability_t`（自由度、限位、最大速率、质量），以及
  `object_describe` / `object_capability` / `object_set_state` / `object_state`；
  `base/verify.c` 增加 6 项断言。基座接口不变
  / **R8 self-description**: the object side gains `object_state_t` and
  `object_capability_t` plus four accessors; `base/verify.c` adds 6 assertions.
  The base interface is unchanged
- **参照系由物件自己解（设计决定 15，方案 A）**：新增 `object_resolve()`，沿参照链
  直读参照系物件的运动，物件之间直连、不经过基座；成环与断链由物件侧拒绝。
  `base/verify.c` 增加 6 项断言。参考实现只做平移合成，尚不按参照系姿态旋转
  / **Objects resolve their own frame (decision 15, option A)**: new
  `object_resolve()` walks the frame chain reading each frame object's motion
  directly, object to object, never through the base; cycles and broken chains
  are rejected on the object side. `base/verify.c` adds 6 assertions. The
  reference only composes translation and does not yet rotate by orientation
- **执行模型（设计决定 16）**：新增 `object_run(now_ns)` 与 `object_step_count()`，
  按各物件到达时申报的 `period_ns` 推进其运动环，把解算出的运动积分进位置；
  时间由调用者给出，基座不调度、不计时。`base/verify.c` 增加 6 项断言（合成时钟）
  / **Execution model (decision 16)**: new `object_run(now_ns)` and
  `object_step_count()` advance each object's motion loop at the `period_ns` it
  declared on arrival, integrating resolved motion into position; time is
  supplied by the caller and the base neither schedules nor keeps time.
  `base/verify.c` adds 6 assertions (synthetic clock)

### Changed / 变更

- **确立统一模型：一切皆是运动物件**。大脑不再是一个独立类别，而是「恰好正在
  驱动基座」的普通接线 —— 没有「大脑槽」，换大脑与物件热插拔是同一个机制，
  基座对称且不授权；**基座是唯一的非物件**（纯函数）
  / **Unified model established: everything is a motion object.** A brain is no
  longer a separate category but simply the ordinary wire that happens to be
  driving the base — there is no "brain slot", replacing a brain is the same
  mechanism as object hot-plug, and the base is symmetric and grants no
  authority; **the base is the only non-object** (a pure function)
- `base_attach` → `base_arrive`，`base_detach` → `base_depart`：
  接线不再由代码「接入」，而是由总线驱动上报「到达」——明确了接线
  来自发现事件，是唯一来源
  / `base_attach` → `base_arrive` and `base_detach` → `base_depart`: wiring is
  no longer "attached" by code but reported as an arrival by a bus driver —
  making discovery the single source of wiring
- 设计模型从「进程/文件」式内核彻底转向「一切皆运动」基座模型
  / The design model moved completely from a process/file-style kernel to the "everything is motion" base model
- 基座定位重新确立为**无状态**：不保存世界信息，只持有接线
  / The base is re-established as **stateless**: it stores no world information, only wiring

### Fixed / 修复

- **接线句柄复用会让死句柄复活**。此前 `wire_t` 就是槽位下标，旧大脑
  离开后新物件立刻复用同一槽位，于是「已离开的句柄」重新变得有效，
  旧大脑会对着陌生物件下指令。在热插拔系统里这是事故级缺陷。
  现句柄编码为 `(世代号 << 16) | 槽位`，世代单调递增，离开的句柄
  永远只是个死句柄，不会再指向任何东西。
  / **Wire handle reuse resurrected dead handles.** `wire_t` used to be a slot
  index, so a departed brain's slot was immediately reused and the "dead"
  handle became valid again — the old brain would then command a stranger. In
  a hot-plug system that is an accident-class defect. Handles now encode
  `(generation << 16) | slot` with a monotonic generation, so a departed
  handle stays dead forever.

- **引导入口必为 32 位代码**。`XEN_ELFNOTE_PHYS32_ENTRY` 的 `PHYS32`
  即「32 位入口」，QEMU 进入时 CPU 处于 32 位保护模式。此前入口按
  64 位编写，`REX` 前缀被当成 `dec`，`lea` 退化为「取位移」，导致
  `rsp`/`cr3` 指向错误地址、内核在 `base_init` 处静默停住。
  现改为 32 位 stub 完成 PAE → CR3 → LME → PG → 远跳转的切换。
  / **The boot entry must be 32-bit code.** `PHYS32` in
  `XEN_ELFNOTE_PHYS32_ENTRY` means a 32-bit entry point; QEMU enters in
  32-bit protected mode. The entry was previously written as 64-bit
  code, so `REX` prefixes were decoded as `dec` and `lea` degenerated
  into "take the displacement", leaving `rsp`/`cr3` pointing at the
  wrong addresses and the kernel silently stalling in `base_init`. It
  is now a 32-bit stub doing PAE → CR3 → LME → PG → far jump.
- **可写段必须文件承载**。纯 `NOBITS` 的 `.bss` 自成一 ELF 段时
  `FileSiz = 0`，QEMU 的 PVH 加载器不会映射它，写它直接页错误。
  现把 `.bss` 与栈并入文件承载的 `.data` 段。
  / **The writable segment must be file-backed.** A purely `NOBITS`
  `.bss` forming its own ELF segment has `FileSiz = 0`; QEMU's PVH
  loader never maps it, so writing to it faults. `.bss` and the stack
  are now folded into the file-backed `.data` segment.
- **栈不得与数据同址**。此前栈顶与接线表起点重合，`base_init` 的返回
  地址入栈即被清零。现栈排在可写段最末尾、`__bss_end` 之后。
  / **The stack must not share memory with data.** The top of the stack
  used to coincide with the start of the wire table, so `base_init`'s
  return address was zeroed as soon as it was pushed. The stack now
  sits at the very end of the writable segment, after `__bss_end`.

### Removed / 移除

- 命令态 / 自报态双表模型（与无状态基座冲突）/ Dual-table model (conflicts with the stateless base)
- 时间轮记账与「到点置静止」/ Time-wheel ledger and "set to still at expiry"
- 运动学拓扑、双重 deadline、故障检测（均属大脑职责）
  / Kinematic topology, dual deadlines, fault detection (all belong to the brain)
- 旧的内核骨架（`kernel/core/sched.c`、`ipc.c`、`cap.c`）标记为废弃
  / Legacy kernel skeleton marked deprecated

### Deprecated / 废弃

- `kernel/include/hal.h`、`cap.h`、`ipc.h`、`mm.h`、`sched.h` — 沿用通用微内核假设，待重写
  / Based on generic microkernel assumptions; to be rewritten

---

## 版本号规则 / Versioning

| 版本 / Version | 含义 / Meaning |
|---|---|
| `0.x` | 设计阶段，接口不稳定 / Design stage, unstable interface |
| `1.0.0` | 基座语义冻结、首个可运行目标 / Base semantics frozen, first runnable target |