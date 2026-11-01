# 安全策略 / Security Policy

[English below](#english)

## 支持范围 / Supported versions

liteOS 尚处于**原型阶段（0.x）**，没有发布过带安全支持的稳定版本。
所有安全相关结论以当前的 `main` 分支为准。

*liteOS is in **prototype (0.x)**; no stable release has security support.
All security statements below refer to the current `main` branch.*

| 分支 / Branch | 支持 / Supported |
|---|---|
| `main`（最新提交） | 是 / Yes |
| 任何 `0.x` 标签 | 否 / No |

## 报告漏洞 / Reporting a vulnerability

**请勿用公开 Issue 报告安全问题。**
*Please do not open a public issue for security problems.*

优先使用 GitHub 的私密报告通道：
**Security → Advisories → Report a vulnerability**

*Preferred channel: GitHub's private advisory form —
**Security → Advisories → Report a vulnerability**.*

若该通道不可用，可联系仓库维护者（见 `CODEOWNERS`）私下沟通。
*If unavailable, contact the maintainers listed in `CODEOWNERS` privately.*

请在报告中给出 / *Please include*：

1. 受影响的架构与提交号 / Affected architecture and commit
2. 复现步骤（QEMU 命令行或最小补丁）/ Reproduction steps (QEMU command line or minimal patch)
3. 你判断的影响范围 / Your assessment of impact
4. 是否已知会被公开 / Whether it is already public elsewhere

我们**无法承诺 SLA**（本项目不是雇佣关系下的产品），
但会对每份报告给出书面回应，并在修复后于 CHANGELOG 记名致谢（除非你要求匿名）。
*There is **no SLA** — this is not a paid product — but every report gets a
written response and, after a fix, credit in the CHANGELOG unless you ask to stay anonymous.*

---

## 安全现状：这是原型，不是安全产品

Security status: this is a prototype, not a secure product.

以下不是"待办事项"，而是**当前已实测确认**的事实。
它们记录在 [docs/assessment.md](assessment.md) 中，并可由内核自己在串口输出复现。

*The following are not TODOs but **measured facts**, recorded in
[docs/assessment.md](assessment.en.md) and reproducible from the kernel's own
serial output.*

### x86_64

```
cr0 = 0x0000000080000011  (PG=1  WP=0)
cr4 = 0x0000000000000020  (PAE=1)
pd[0] = 0x00000000000000e3  (P=1 RW=1 US=0 NX=0)
```

分页已开启，但 `CR0.WP` 未设置，页全部**可写且可执行**，没有用户态映射，
没有 NX 隔离。内核代码、内核数据与上层大脑没有 MMU 边界。

*Paging is on, but `CR0.WP` is clear and every page is **writable and
executable**, with no user mapping and no NX isolation. There is no MMU
boundary between kernel code, kernel data, and an upper brain.*

### riscv64

```
satp = 0x0000000000000000  (MODE=0  Bare)
```

S 态不做地址翻译，没有页级权限位。上层大脑与基座同权。

*S-mode performs no address translation and has no page permissions. An
upper brain runs with the same privilege as the base.*

### 架构层面的共同缺口 / Shared architectural gaps

- 没有用户态与内核态隔离 / No user/kernel separation
- 没有 W^X / No W^X
- 没有大脑沙箱 / No brain sandbox
- `base.h` 没有任何授权概念：任何持有有效句柄的调用者原则上可命令任意活接线
  （设计决定 13 是**有意**不授权的，正确性由外部的大脑组成承担）
  / `base.h` has no authority concept: any caller holding a valid handle may, in
  principle, command any live wire (decision 13 grants no authority **on purpose**;
  correctness is composed externally by brains)
- 物件申报的限位与最大速率**不被基座强制执行**
  / Declared limits and maximum rates are **not enforced** by the base
- `base/` 与 `object.c` 无锁，不保证 SMP、抢占或中断上下文重入
  / `base/` and `object.c` are lock-free and do not guarantee SMP, preemption, or
  interrupt-context reentrancy
- 构建使用 `-fno-stack-protector`，且没有栈守卫页
  / Built with `-fno-stack-protector`, and no stack guard page

### 因此 / Therefore

liteOS **不是**一个适合承载以下负载的系统：

- 连接不可信输入（网络、用户态、外部设备固件）
- Must 满足安全认证（ISO 26262 / IEC 61508 / DO-178C / Common Criteria 等）
- 运行来自第三方的、未经审查的"大脑"代码

*liteOS is **not** suitable for untrusted input, safety certifications, or
running unreviewed third-party brain code.*

## 供应链 / Supply chain

- 依赖：clang/lld、nasm、QEMU、GNU Make。无第三方 C 库，无包管理器依赖。
  / Dependencies: clang/lld, nasm, QEMU, GNU Make. No third-party C libraries and no
  package-manager dependencies.
- 构建可复现性尚未验证（未固定工具链版本）。
  / Build reproducibility is not yet verified (toolchain versions are not pinned).
- 没有签名发布物。/ No signed artifacts are published.
