# liteOS

> 一切皆运动。基座只管运动控制 —— 无状态，可被任意大脑骑乘。
>
> *Everything is motion. The base manages only motion control — stateless, and any brain can ride it.*

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Language: C](https://img.shields.io/badge/language-C11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Status: Design](https://img.shields.io/badge/status-design-orange.svg)](#项目状态--project-status)

---

## 这是什么 / What is this

liteOS 不是"一个操作系统"，而是一个**基座**：一个只管运动控制、无状态、可被任意上层大脑骑乘的底座。

*liteOS is not "an operating system" but a **base**: a stateless foundation that does exactly one thing — motion control — and can be ridden by any replaceable brain.*

| 层 / Layer | 角色 / Role | 特征 / Property |
|---|---|---|
| **上层大脑 / Brain** | 意图、策略、智能 | 可热插拔 / hot-pluggable |
| **基座 / Base** | 只做运动控制 / motion control only | 唯一不变、无状态 / invariant, stateless |
| **物件 / Object** | 物理世界 / physical world | 即插即用 / plug-and-play |

---

## 核心思想 / Core idea

**本体论 / Ontology**

1. 万事万物都是运动 / Everything is motion
2. 没有运动，物件不存在 / Without motion, an object does not exist
3. **静止是一种相对的运动 / Stillness is relative motion**

第 3 条是整个设计的支点：静止不是运动的一个取值，而是**一次比较的结果**。比较需要**参照系**，参照系是**信息**；基座不保存信息 ⇒ **判定「静止/运动」不属于基座**。

*The third point is the fulcrum: stillness is not a value of motion but the result of a comparison. A comparison needs a **frame of reference**, which is **information**; the base stores no information ⇒ deciding "still or moving" is not the base's job.*

**唯一原语 / Single primitive**

```
施加运动(物件, 运动, 参照系, 时长)
apply_motion(object, motion, frame, duration)
```

没有「停止」这个指令 —— 静止就是以参照系的运动为自身的运动。
*There is no "stop" command — stillness is simply motion relative to a moving frame.*

---

## 架构 / Architecture

```
              上层大脑（可热插拔）
              ╱                    ╲
   运动控制（唯一通路，自描述）      其他一切信息（直连）
            ╱                          ╲
           ▼                            ▼
    ┌─────────────┐                 ┌────────┐
    │   基座       │   施加运动       │  物件   │
    │  无状态      ├────────────────▶│        │
    └─────────────┘                 └────────┘
```

运动控制是这个系统的**唯一窄腰**，正如 IP 是网络的窄腰。
*Motion control is the single narrow waist of this system, just as IP is for networks.*

---

## 快速开始 / Quick start

构建与运行（x86_64，QEMU）/ Build and run (x86_64, QEMU):

```sh
make ARCH=x86_64
make run
```

串口会输出基座自检结果 / The serial console prints the base self-test:

```
=== liteOS ===
[boot] magic = 0x00001580
[boot] info  = 0x00000560

--- base self-test ---
  A = wire 1, B = wire 2

  ok   A moves relative to the world
  ...
  failures = 0
```

需要 / Requirements: clang + lld, nasm, QEMU。工具链检查见
`tools/check-toolchain.sh`。引导细节（PVH、32→64 位切换）见
[docs/boot.md](docs/boot.md)。

*Toolchain check: `tools/check-toolchain.sh`. Boot details (PVH, the
32→64 transition): [docs/boot.en.md](docs/boot.en.md).*

---

## 文档 / Documentation

| 文档 / Document | 说明 / Description |
|---|---|
| [docs/design.md](docs/design.md) | 设计文档（中文）/ Design document (Chinese) |
| [docs/design.en.md](docs/design.en.md) | Design document (English) / 设计文档（英文） |
| [docs/boot.md](docs/boot.md) | 引导路径（中文）/ Boot path (Chinese) |
| [docs/boot.en.md](docs/boot.en.md) | Boot path (English) / 引导路径（英文） |
| [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) | 贡献指南 / Contributing guide |
| [docs/CHANGELOG.md](docs/CHANGELOG.md) | 变更记录 / Changelog |
| [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) | 行为准则 / Code of conduct |

---

## 项目状态 / Project status

- [x] 本体论与核心原语确立 / Ontology and primitive established
- [x] 基座无状态边界（接线 vs 信息）/ Statelessness boundary (wiring vs information)
- [x] 运动控制事件的确切形状 / Exact shape of motion events
- [x] 首个可运行目标（x86_64 + QEMU，串口自检）/ First runnable target (x86_64 + QEMU, serial self-test)
- [ ] 接线表规模与更新语义 / Wiring table scale and update semantics
- [ ] 第二个处理器架构 / A second processor architecture

---

## 许可证 / License

[MIT](LICENSE)