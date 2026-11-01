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
- **执行层接入真实时钟**：PIT 100 Hz 节拍在 `kmain.c` 主循环中驱动 `object_run`，
  中断处理只累加节拍；启动后物件随真实时间持续运动，内核不再停在 `[idle] halting`
  / **Execution layer on a real clock**: 100 Hz PIT ticks drive `object_run` from
  the main loop in `kmain.c`, while the interrupt handler only counts ticks;
  objects now keep moving in real time and the kernel no longer stops at
  `[idle] halting`
- **运行时热插拔**：新增 `pci_poll()`，主循环每 100 ms 重扫 PCI 总线并与上次
  结果比对，新设备到达（`object_plug`）、消失设备离开（`object_unplug`）；
  拔出请求通过 QEMU PIIX4 ACPI 热插拔寄存器（0xAE00）完成弹出。
  新增 `tools/hotplug-test.sh`，运行中插入并拔出 e1000 网卡验证
  / **Runtime hot-plug**: new `pci_poll()` rescans the PCI bus every 100 ms from
  the main loop and diffs against the previous scan — new devices arrive, vanished
  ones depart; removal requests are completed through the QEMU PIIX4 ACPI hotplug
  registers (0xAE00). New `tools/hotplug-test.sh` adds and removes an e1000 NIC
  while the kernel runs
- **第二个处理器架构 riscv64**（`make ARCH=riscv64`，QEMU virt + OpenSBI）：
  新增 `arch/riscv64/`（入口、链接脚本、16550 串口、time CSR 驱动执行层）。
  `base/` 未改一行，全部验证在 riscv64 上通过；Makefile 的架构相关选项集中到一处
  / **Second processor architecture, riscv64** (`make ARCH=riscv64`, QEMU virt +
  OpenSBI): new `arch/riscv64/` (entry, linker script, 16550 UART, time CSR
  driving the execution layer). `base/` is unchanged and all verification passes
  on riscv64; architecture-specific Makefile options are gathered in one place
- **参照系姿态旋转**：`object_resolve()` 每升一级先按参照系的姿态四元数旋转运动
  （线 0..2、角 3..5），再叠加参照系自身的运动；定点乘法用 128 位中间积。
  `base/verify.c` 增加 4 项断言（不转、转 180°、转 90°、相对旋转参照系静止），
  x86_64 与 riscv64 均通过
  / **Frame orientation**: at each level `object_resolve()` first rotates the
  motion (linear 0..2, angular 3..5) by the frame's orientation quaternion, then
  adds the frame's own motion; fixed-point multiplication uses a 128-bit
  intermediate. `base/verify.c` adds 4 assertions, passing on x86_64 and riscv64
- **符合性标准 R1–R3 自动检验**：`base/verify.c` 增加 6 项断言（零运动即静止、
  事件到期后继续运动、申报属性可读且离开后失效）；新增 `object_declared()`，
  物件侧保存到达时申报的 `unit` / `period_ns` 供大脑直读。R1–R8 现均已检验或由类型保证
  / **Automated R1–R3 conformance checks**: `base/verify.c` adds 6 assertions
  (zero motion means still, motion continues after the event expires, declared
  properties are readable and gone after departure); new `object_declared()`
  keeps the arrival-time `unit` / `period_ns` on the object side for the brain.
  R1–R8 are now all tested or type-enforced
- **riscv64 总线发现**：PCI 驱动移到两种架构共用的 `drivers/pci.c`；配置空间访问与
  热插拔弹出改由架构层的 `arch_pci_read32()` / `arch_pci_eject_pending()` 提供
  （x86_64 端口 I/O，riscv64 ECAM 0x30000000）。QEMU virt 上发现主桥、e1000、virtio-net
  / **riscv64 bus discovery**: the PCI driver moves to `drivers/pci.c`, shared by both
  architectures; configuration-space access and hotplug eject are supplied by the
  architecture layer through `arch_pci_read32()` / `arch_pci_eject_pending()`
  (x86_64 port I/O, riscv64 ECAM at 0x30000000). On QEMU virt it finds the host
  bridge, an e1000 and a virtio-net device
- **riscv64 定时器中断**：新增 `arch/riscv64/trap.c` 与 `traps.S`（`stvec` 直接模式，
  保存全部寄存器后调用 `trap_dispatch`）。定时器经 SBI TIME 扩展（EID 0x54494D45）
  编程 —— OpenSBI 用 PMP 把 CLINT（0x02000000）设为 S 态无访问权限，直读写分别得到
  scause 5 与 scause 7，实测 pmpcfg0 确认。主循环 `wfi` 等中断；迟到运行按
  设计决定 16 只推进一次、不补跑，因此节拍与步数可能相差一小步，串口会同时
  打印二者并说明原因
  / **riscv64 timer interrupts**: new `arch/riscv64/trap.c` and `traps.S` (`stvec`
  in direct mode; all registers are saved before calling `trap_dispatch`). The timer
  is programmed through the SBI TIME extension (EID 0x54494D45) — OpenSBI marks the
  CLINT (0x02000000) as S-mode no-access through PMP, so a direct read or write
  raises scause 5 or scause 7 respectively, confirmed by reading pmpcfg0. The main
  loop waits in `wfi`; under decision 16 a late run steps once rather than catching
  up, so ticks and steps may differ slightly and both are printed

- **安全、可靠性与性能评估**：新增目标内核基准与压力测试，覆盖合法/拒绝路径、
  1 层与 8 层参照链、全部可用槽位的最坏推进、表满故障关闭、伪造句柄拒绝和
  20,000 次插拔循环；新增 `tools/bench.sh` 与中英文评估报告。QEMU 实测最坏推进
  约占一个 100 Hz 节拍的 1% 至 2%
  / **Security, reliability, and performance assessment**: new in-kernel benchmarks
  and stress tests cover accepted/rejected paths, one- and eight-object frame chains,
  a worst pass over every available slot, fail-closed capacity exhaustion, forged
  handles, and 20,000 plug/unplug cycles. `tools/bench.sh` and bilingual assessment
  reports reproduce the results; the measured worst pass consumes about 1% to 2% of
  a 100 Hz tick under QEMU
- **保护状态自报**：x86_64 打印 CR0/CR4/CR3 与页目录权限；riscv64 打印 satp。
  当前证据明确显示 x86_64 为可写可执行恒等映射且 `CR0.WP=0`，riscv64 为 Bare
  模式，因此尚无生产级内存或上层大脑隔离
  / **Protection-state self-reporting**: x86_64 prints CR0/CR4/CR3 and page-directory
  permissions, while riscv64 prints satp. The evidence shows writable/executable
  identity mappings with `CR0.WP=0` on x86_64 and Bare mode on riscv64, so production
  memory isolation and upper-brain sandboxing are not yet present

### Changed / 变更

- **基座不再接收任何自述属性（设计决定 17）**：删除 `wire_desc_t`，`base_arrive()`
  改为无参数；接线表每槽只剩世代号与在用标记。`unit` / `period_ns` 只保存在物件侧，
  大脑经 `object_declared()` 直读。`object_plug()` 签名不变
  / **The base no longer receives any self-declared properties (decision 17)**:
  `wire_desc_t` is removed and `base_arrive()` takes no arguments; each wiring-table
  slot holds only a generation and an in-use flag. `unit` / `period_ns` live only on
  the object side, read by the brain through `object_declared()`. `object_plug()`
  keeps its signature
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

- **统一模型自检遗留三个物件**：`brain2`、`hand`、`foot` 在测试结束后未归还，
  会静默减少后续可用容量。现自检结束时全部拔出，基准入口容量只反映真实 PCI
  设备占用
  / **The unified-model self-test leaked three objects**: `brain2`, `hand`, and
  `foot` remained plugged after the test, silently reducing later capacity. They are
  now released, so benchmark entry capacity reflects only real PCI devices
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