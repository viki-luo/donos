#!/usr/bin/env bash
# check-toolchain.sh — 检查构建与运行所需的真实工具链
#
# 与 Makefile 实际使用的工具保持一致：clang + lld。
# 只读检查，不改动任何文件。
set -e

# MSYS2 基础环境需要显式补全 PATH（Linux/macOS 上 /ucrt64 不存在，忽略即可）
if [ -d /ucrt64/bin ]; then
        export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH
fi

# want <名称> <候选命令...>：任一候选存在即通过。
# want <name> <candidate...>: pass if any candidate is on PATH.
want() {
        local name="$1"; shift
        for c in "$@"; do
                if command -v "$c" >/dev/null 2>&1; then
                        printf '  ok   %s (%s)\n' "$name" "$c"
                        return 0
                fi
        done
        printf '  MISS %s (候选: %s)\n' "$name" "$*"
}

echo "=== 必需工具 / required ==="
want clang        clang
want lld          ld.lld
want nm           llvm-nm nm
want size         llvm-size size
want objcopy      llvm-objcopy objcopy
want nasm         nasm
want make         make
want qemu-x86     qemu-system-x86_64
want qemu-riscv   qemu-system-riscv64
want timeout      timeout

echo ""
echo "=== 版本 / versions ==="
clang --version | head -1
ld.lld --version
nasm -v
qemu-system-x86_64 --version | head -1
qemu-system-riscv64 --version | head -1
make --version | head -1
