#!/usr/bin/env bash
# disasm.sh — 反汇编关键函数，检查字符串访问方式
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
if [ -d /ucrt64/bin ]; then export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH; fi

BIN=build/x86_64/liteos.bin

echo "=== kernel_main 反汇编 ==="
llvm-objdump -d --no-show-raw-insn "$BIN" | sed -n '/<kernel_main>:/,/<.*>:/p' | head -60
