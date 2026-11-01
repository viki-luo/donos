# 贡献指南 / Contributing

完整的贡献指南在 **[docs/CONTRIBUTING.md](docs/CONTRIBUTING.md)**（双语）。
这个文件只是一个入口，因为 GitHub 在新建 Issue/PR 时会主动链接根目录的
`CONTRIBUTING.md`。

*The full guide lives in **[docs/CONTRIBUTING.md](docs/CONTRIBUTING.md)** (bilingual).
This file is only a pointer, because GitHub links the repository-root
`CONTRIBUTING.md` when opening issues and pull requests.*

---

## 三条必须先知道的事 / Three things to know first

1. **五条设计律不接受例外**（基座无状态、接线非信息、唯一原语、静止相对、
   运动是唯一窄腰）。违反任一条的 PR 不会被合并。
   *The five design laws admit no exceptions. A PR violating any of them will not
   be merged.*

2. **文档必须中英双语。** 新增功能要同时更新两种语言。
   *Documentation must be bilingual. New features update both languages.*

3. **提交信息遵循 [Conventional Commits](https://www.conventionalcommits.org/)。**
   *Commits follow Conventional Commits.*

---

## 快速上手 / Quick start

```sh
git clone <repo>
cd os

# 工具链：clang + lld、nasm、QEMU、GNU Make
tools/check-toolchain.sh

# 构建（两个架构都应通过）
make ARCH=x86_64
make ARCH=riscv64

# 在 QEMU 中运行并留下串口日志
tools/run-trace.sh            # → build/serial.log

# 完整测量（安全、可靠性、性能）
tools/bench.sh
```

## 提交前自查 / Before you open a PR

```sh
make clean && make ARCH=x86_64 && make ARCH=riscv64
tools/bench.sh
grep -q 'failures = 0'      build/bench-x86_64.log
grep -q 'failures = 0'      build/bench-riscv64.log
grep -q 'bench failures = 0' build/bench-x86_64.log
grep -q 'bench failures = 0' build/bench-riscv64.log
```

CI 会执行同样的检查（`.github/workflows/`）。

*CI runs the same checks.*

## 行为准则 / Code of conduct

参与本项目即表示同意遵守 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。
*By participating you agree to [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).*
