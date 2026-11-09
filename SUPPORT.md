# 获取帮助 / Getting help

[English below](#english)

## 先用这些资源 / Try these first

| 你想做的事 | 看这里 |
|---|---|
| 了解项目是什么 | [README.md](README.md) |
| 理解设计理念与 17 条决定 | [docs/design.md](docs/design.md) |
| 构建并在 QEMU 中运行 | [README.md § 快速开始](README.md#快速开始--quick-start) |
| 引导为什么这样写（PVH、32→64 位） | [docs/boot.md](docs/boot.md) |
| 接入自己的物件 | [docs/conformance.md](docs/conformance.md) |
| 看性能与安全实测结论 | [docs/assessment.md](docs/assessment.md) |
| 报告安全问题 | [SECURITY.md](SECURITY.md) |

## 提问与讨论 / Questions and discussion

- **用法问题、设计疑问** → 开一个
  [Design discussion](https://github.com/viki-luo/donos/issues/new?template=design_discussion.md)
- **可复现的缺陷** → 开一个
  [Bug report](https://github.com/viki-luo/donos/issues/new?template=bug_report.md)
- **想要的能力** → 开一个
  [Feature request](https://github.com/viki-luo/donos/issues/new?template=feature_request.md)
- **安全漏洞** → **不要**开公开 Issue，走 [SECURITY.md](SECURITY.md)

> 上面链接里的 `viki-luo/donos` 需替换为本仓库的实际地址。
> *Replace `viki-luo/donos` in the links above with the real repository path.*

## 提问前请准备好 / Before asking, please have ready

1. `make ARCH=<arch>` 的**完整输出**（尤其是第一条错误）
   / Full `make` output, especially the first error
2. 架构（`x86_64` / `riscv64`）、宿主系统、QEMU 版本
   / Architecture, host OS, QEMU version
3. 串口日志（`tools/run-trace.sh` 会写到 `build/serial.log`）
   / Serial log (`tools/run-trace.sh` writes `build/serial.log`)
4. 你已经试过什么 / What you already tried

## 本项目的边界 / Scope of this project

为了让讨论有效，先明确**不接受**的请求：

**This project will not accept**:

- 引入第二种运动原语（例如「停止」「复位」「设置模式」）
  / A second motion primitive (e.g. "stop", "reset", "set mode")
- 把世界状态、位姿、日志、文件系统放进基座
  / World state, pose, logs, or a filesystem inside the base
- 在 `motion_event_t` 里增加不透明载荷或模式字段
  / Opaque payloads or mode fields in `motion_event_t`
- 给基座增加「大脑槽」或任何特权接线
  / A "brain slot" or any privileged wire in the base

理由见 [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) 的设计律与
[docs/design.md](docs/design.md) §8。

## 响应预期 / What to expect

这是一个**业余时间维护的原型项目**，没有 SLA。
维护者会尽量回应，但请允许数周量级的延迟。

*This is a prototype maintained in spare time, with no SLA. Maintainers try to
respond, but please allow delays on the order of weeks.*

---

## English

| I want to... | Read this |
|---|---|
| Understand the project | [README.md](README.md) |
| Understand the design and its 17 decisions | [docs/design.en.md](docs/design.en.md) |
| Build and run under QEMU | [README.md § Quick start](README.md#快速开始--quick-start) |
| Know why boot code is written this way | [docs/boot.en.md](docs/boot.en.md) |
| Attach my own object | [docs/conformance.en.md](docs/conformance.en.md) |
| See measured performance and security results | [docs/assessment.en.md](docs/assessment.en.md) |
| Report a vulnerability | [SECURITY.md](SECURITY.md) |

**Questions and design debates** → open a
[Design discussion](https://github.com/viki-luo/donos/issues/new?template=design_discussion.md).
**Reproducible defects** → open a
[Bug report](https://github.com/viki-luo/donos/issues/new?template=bug_report.md).
**Requested capabilities** → open a
[Feature request](https://github.com/viki-luo/donos/issues/new?template=feature_request.md).
**Security vulnerabilities** → do **not** open a public issue; follow
[SECURITY.md](SECURITY.md).
