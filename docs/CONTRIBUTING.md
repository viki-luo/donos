# 贡献指南 / Contributing Guide

感谢你对 liteOS 的关注！/ *Thanks for your interest in liteOS!*

liteOS 是一个围绕单一理念构建的项目：**一切皆运动，基座只管运动控制**。因此，贡献之前请先理解这条设计律，它决定了什么代码可以被接受。

*liteOS is built around a single idea: **everything is motion, and the base does only motion control**. Before contributing, please understand this design law — it determines what code can be accepted.*

---

## 设计律 / Design laws

所有贡献必须遵守以下不变量。违反任何一条的 PR 将不被接受。

*All contributions must respect these invariants. A PR violating any of them will not be accepted.*

| # | 律 / Law | 含义 / Meaning |
|---|---|---|
| 1 | 基座无状态 / The base is stateless | 基座不保存世界信息 / The base stores no world information |
| 2 | 接线还是信息 / Wiring vs information | 基座可持有接线，不可持有信息 / Base may hold wiring, never content |
| 3 | 唯一原语 / Single primitive | 只有「施加运动」，没有「停止」/ Only `apply_motion`, never `stop` |
| 4 | 静止是相对的 / Stillness is relative | 判定属于大脑，施加属于基座 / Deciding belongs to the brain, applying to the base |
| 5 | 运动是唯一窄腰 / Motion is the narrow waist | 其他一切信息不经过基座 / All other information bypasses the base |

---

## 开发环境 / Development environment

### 当前状态 / Current status

项目是可运行的原型：`base/` 在 x86_64 与 riscv64 上都能构建并在 QEMU 中启动，
自检与压力测试通过。仍在演进，接口（`base.h`）在 0.x 阶段不稳定。
*The project is a runnable prototype: `base/` builds and boots under QEMU on both
x86_64 and riscv64, and self-tests plus stress tests pass. Interfaces (`base.h`)
remain unstable during 0.x.*

### 工具链 / Toolchain

| 组件 / Component | 用途 / Purpose |
|---|---|
| `clang` + `ld.lld` | 交叉编译与链接到裸机 ELF / cross-compile and link to bare-metal ELF |
| `llvm-objcopy` `llvm-nm` `llvm-size` | 构建产物检查 / artifact inspection |
| `nasm` | x86_64 引导代码 / x86_64 boot code |
| `qemu-system-x86_64` `qemu-system-riscv64` | 模拟运行 / Emulation |
| GNU Make | 构建 / Build |

先跑 `tools/check-toolchain.sh` 确认环境，再用 `make check` 跑完整门禁。
*Run `tools/check-toolchain.sh` to confirm the environment, then `make check` for
the full gate.*

---

## 代码风格 / Coding style

- **语言 / Language**：C11，`-std=gnu11`，freestanding
- **缩进 / Indentation**：8 空格，不使用 Tab / 8 spaces, no tabs
- **命名 / Naming**：
  - 函数、变量 / functions, variables：`snake_case`
  - 类型 / types：`snake_case_t`（如 `motion_event_t`）
  - 宏 / macros：`UPPER_SNAKE_CASE`
- **头文件保护 / Header guards**：`LITEOS_<PATH>_H`
- **注释 / Comments**：中文或英文均可，同一文件内保持一种语言 / Either Chinese or English, but stay consistent within a file

---

## 提交规范 / Commit convention

本项目遵循 [Conventional Commits](https://www.conventionalcommits.org/)。

*This project follows [Conventional Commits](https://www.conventionalcommits.org/).*

```
<type>(<scope>): <subject>

[optional body]

[optional footer]
```

**type**：

| 类型 / Type | 说明 / Description |
|---|---|
| `feat` | 新功能 / New feature |
| `fix` | 修复 / Bug fix |
| `docs` | 文档 / Documentation |
| `refactor` | 重构 / Refactoring |
| `test` | 测试 / Tests |
| `build` | 构建系统 / Build system |
| `chore` | 杂项 / Chores |

**示例 / Examples**：

```
feat(base): implement stateless apply_motion primitive
docs(design): clarify wiring vs information boundary
fix(brain): correct frame resolution on attach
```

---

## 分支与 PR / Branches and pull requests

1. 从 `main` 拉出特性分支 / *Branch off `main`*：
   - `feat/<short-description>`
   - `fix/<short-description>`
   - `docs/<short-description>`
2. 提交前确保 / *Before submitting*：
   - `make check` 通过（两架构构建 + 自检 + 测量）/ *`make check` passes (both architectures, self-test, measurements)*
   - 未违反设计律 / *No design law violated*
   - 文档已同步更新（中英双语）/ *Docs updated (bilingual)*
3. 填写 PR 模板 / *Fill in the PR template*
4. 至少一位维护者审查通过 / *At least one maintainer approval*

---

## 文档规范 / Documentation convention

**本项目要求中英文双语文档。**
*This project requires bilingual (Chinese + English) documentation.*

- 面向用户的主要文档（`README.md`、`docs/design.md`）采用**中英对照**格式
- 中文件与英文件可分列，也可同页对照
- 新增功能必须同时更新中英两版文档
- *User-facing documents (`README.md`, `docs/design.md`) use a **bilingual** format. New features must update both language versions.*

---

## 成为维护者 / Becoming a maintainer

持续贡献者可由现有维护者提名成为维护者。
*Consistent contributors may be nominated by existing maintainers.*

---

## 行为准则 / Code of conduct

参与本项目即表示同意遵守 [CODE_OF_CONDUCT.md](../CODE_OF_CONDUCT.md)。
*By participating, you agree to abide by [CODE_OF_CONDUCT.md](../CODE_OF_CONDUCT.md).*