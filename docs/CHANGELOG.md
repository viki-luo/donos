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

### Changed / 变更

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