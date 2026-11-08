# liteOS

> 一切皆运动。基座只管运动控制 —— 无状态，可被任意大脑骑乘。
>
> *Everything is motion. The base manages only motion control — stateless, and any brain can ride it.*

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Language: C](https://img.shields.io/badge/language-C11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Status: Prototype](https://img.shields.io/badge/status-prototype-orange.svg)](#项目状态--project-status)
[![Docs: bilingual](https://img.shields.io/badge/docs-%E4%B8%AD%E8%8B%B1%E5%8F%8C%E8%AF%AD%20%7C%20bilingual-informational.svg)](docs/README.md)
[![Security: prototype, no isolation](https://img.shields.io/badge/security-no%20isolation-red.svg)](SECURITY.md)

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

串口会输出平台初始化、总线发现与自检结果（节选）/
The serial console prints platform init, bus discovery and the self-test (excerpt):

```
=== liteOS ===
--- platform ---
  idt   installed (48 vectors)
  irq   enabled, timer ticking
--- bus discovery (PCI) ---
  00.0  8086:1237  [bridge 06/00]  wire 1
  ...
  03.0  8086:100e  [network 02/00]  wire 6
  discovered 6 object(s)
--- base self-test ---
  ok   A moves relative to the world
  ...
  failures = 0
```

关键：管线框里的接线（`wire 1..6`）来自 PCI 扫描，不是测试代码手写的。
/ The wired objects (`wire 1..6`) above come from a real PCI scan, not from
test code.

需要 / Requirements: clang + lld, nasm, QEMU。工具链检查见
`tools/check-toolchain.sh`。引导细节（PVH、32→64 位切换）见
[docs/boot.md](docs/boot.md)。

运行两种架构的安全、可靠性与性能测量 / Run the security, reliability,
and performance measurements on both architectures:

```sh
make ARCH=x86_64
make ARCH=riscv64
tools/bench.sh

# 等价的一条命令（CI 跑的就是它）
make check
```

评估结论必须同时阅读性能数字与安全限制：满表推进的 p99.9 与最大值约占
100 Hz 节拍的 8.95%（x86_64）/ 7.66%（riscv64），这是 QEMU 确定性指令
计数基线（`-icount`），不是芯片纳秒或 WCET 证明；且尚无用户态、
x86_64 完整 W^X 或上层大脑沙箱。**每条结论都标注证据等级（E1 实测 /
E2 门禁 / E3 仅论证）**，详见 [中文评估](docs/assessment.md) /
[English assessment](docs/assessment.en.md) 与 [SECURITY.md](SECURITY.md)。

*Toolchain check: `tools/check-toolchain.sh`. Boot details (PVH, the
32→64 transition): [docs/boot.en.md](docs/boot.en.md). To verify the
exception handler, build once with `make FAULT_TEST=1`.*

---

## 文档 / Documentation

| 文档 / Document | 说明 / Description |
|---|---|
| [docs/design.md](docs/design.md) | 设计文档（中文）/ Design document (Chinese) |
| [docs/design.en.md](docs/design.en.md) | Design document (English) / 设计文档（英文） |
| [docs/boot.md](docs/boot.md) | 引导路径（中文）/ Boot path (Chinese) |
| [docs/boot.en.md](docs/boot.en.md) | Boot path (English) / 引导路径（英文） |
| [docs/conformance.md](docs/conformance.md) | 物件符合性标准（中文）/ Object conformance standard (Chinese) |
| [docs/conformance.en.md](docs/conformance.en.md) | Object conformance standard (English) / 物件符合性标准（英文） |
| [docs/assessment.md](docs/assessment.md) | 安全、可靠性与性能评估（中文）/ Assessment (Chinese) |
| [docs/assessment.en.md](docs/assessment.en.md) | Security, reliability, and performance assessment (English) / 评估（英文） |
| [docs/README.md](docs/README.md) | 文档索引与术语对照 / Documentation index and glossary |
| [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) | 贡献指南（完整版）/ Contributing guide (full) |
| [docs/CHANGELOG.md](docs/CHANGELOG.md) | 变更记录 / Changelog |
| [SECURITY.md](SECURITY.md) | 安全策略与**当前安全缺口** / Security policy and current gaps |
| [SUPPORT.md](SUPPORT.md) | 寻求帮助与项目边界 / Getting help and project scope |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 贡献入口（指向 docs/）/ Contributing entry point |
| [CITATION.cff](CITATION.cff) | 如何引用 / How to cite |
| [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) | 行为准则 / Code of conduct |

---

## 项目状态 / Project status

- [x] 本体论与核心原语确立 / Ontology and primitive established
- [x] 基座无状态边界（接线 vs 信息）/ Statelessness boundary (wiring vs information)
- [x] 运动控制事件的确切形状 / Exact shape of motion events
- [x] 首个可运行目标（x86_64 + QEMU，串口自检）/ First runnable target (x86_64 + QEMU, serial self-test)
- [x] 中断与异常处理（IDT/PIC/PIT）/ Interrupts and exception handling
- [x] 总线发现：接线来自真实的 PCI 扫描 / Bus discovery: wiring from a real PCI scan
- [x] 运行时热插拔（轮询 PCI 总线，`tools/hotplug-test.sh` 验证）/ Runtime hot-plug (PCI bus polling, verified by `tools/hotplug-test.sh`)
- [ ] 中断驱动的热插拔（ACPI GPE）/ Interrupt-driven hot-plug (ACPI GPE)
- [ ] 接线表规模与更新语义 / Wiring table scale and update semantics
- [x] 第二个处理器架构：riscv64（`make ARCH=riscv64`，QEMU virt + OpenSBI），`base/` 未改一行 / A second processor architecture: riscv64, with `base/` unchanged
- [x] 双架构安全、可靠性与性能评估（`tools/bench.sh`，中英文报告）/ Dual-architecture security, reliability, and performance assessment
- [ ] 内存与特权隔离（x86_64 W^X/NX，riscv64 Sv39，上层大脑沙箱）/ Memory and privilege isolation
- [ ] 上层大脑（大脑热拔插）/ The upper brain (brain hot-swap)

---

## 许可证 / License

[MIT](LICENSE)