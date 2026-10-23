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

### Changed / 变更

- 设计模型从「进程/文件」式内核彻底转向「一切皆运动」基座模型
  / The design model moved completely from a process/file-style kernel to the "everything is motion" base model
- 基座定位重新确立为**无状态**：不保存世界信息，只持有接线
  / The base is re-established as **stateless**: it stores no world information, only wiring

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