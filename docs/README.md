# 文档索引 / Documentation index

[返回根目录](../README.md) · [Back to root](../README.md)

本目录的每一份文档都有中英两版。**双语是硬要求**，见
[docs/CONTRIBUTING.md](CONTRIBUTING.md)。

*Every document here has a Chinese and an English version. Bilingual docs are
mandatory; see [docs/CONTRIBUTING.md](CONTRIBUTING.md).*

---

## 按目的查找 / Find by purpose

| 我想…… / I want to... | 中文 / Chinese | English |
|---|---|---|
| 知道这个项目是什么 | [../README.md](../README.md) | [../README.md](../README.md) |
| 理解设计理念与 17 条决定 | [design.md](design.md) | [design.en.md](design.en.md) |
| 知道引导为什么这样写 | [boot.md](boot.md) | [boot.en.md](boot.en.md) |
| 接入自己的物件 | [conformance.md](conformance.md) | [conformance.en.md](conformance.en.md) |
| 看安全、可靠性、性能实测 | [assessment.md](assessment.md) | [assessment.en.md](assessment.en.md) |
| 参与贡献 | [CONTRIBUTING.md](CONTRIBUTING.md) | [CONTRIBUTING.md](CONTRIBUTING.md) |
| 看改了什么 | [CHANGELOG.md](CHANGELOG.md) | [CHANGELOG.md](CHANGELOG.md) |
| 报告安全问题 | [../SECURITY.md](../SECURITY.md) | [../SECURITY.md](../SECURITY.md) |
| 寻求帮助 | [../SUPPORT.md](../SUPPORT.md) | [../SUPPORT.md](../SUPPORT.md) |

---

## 阅读顺序建议 / Suggested reading order

1. **[design.md](design.md)** — 为什么基座可以无状态，为什么没有「停止」指令。
   *Why the base can be stateless, and why there is no "stop" command.*
2. **[conformance.md](conformance.md)** — 一个物件要满足什么才能插上来。
   *What an object must satisfy to plug in.*
3. **[boot.md](boot.md)** — 从 PVH 32 位入口到 64 位内核的实际路径。
   *The real path from the PVH 32-bit entry to a 64-bit kernel.*
4. **[assessment.md](assessment.md)** — 已实测的数字，以及**明确没有做到的事**。
   *Measured numbers, and an explicit list of what is **not** done.*

## 文档的诚实性要求 / Honesty requirement for docs

- 实测结论必须给出复现命令；没有复现方式的性能数字不写入文档。
  *Measured claims must ship with a reproduction command.*
- 安全结论必须同时写明**当前缺口**，不得只写目标状态。
  *Security conclusions must state current gaps, not only intended properties.*

## 术语对照 / Glossary

| 中文 | English | 含义 |
|---|---|---|
| 基座 | base | 无状态运动控制函数，唯一的非物件 |
| 物件 | object | 即插即用的运动物 |
| 大脑 | brain | 恰好正在驱动基座的普通接线，可热插拔 |
| 接线 | wire | 端口到物件的通道；基座持有的唯一东西 |
| 参照系 | frame | 另一条接线；`motion` 相对它 |
| 拾取器模型 | pick-and-place | `duration_ns` 是事件有效期，到期后物件继续运动 |
| 施加运动 | apply_motion | 唯一原语 |
