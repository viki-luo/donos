#!/usr/bin/env bash
# 验证 clang 能否生成真正的裸机 ELF
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
if [ -d /ucrt64/bin ]; then export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH; fi

WORK="$ROOT/build/smoke"
mkdir -p "$WORK"

printf 'int f(void) { return 42; }\n' > "$WORK/t.c"

echo "=== clang 版本 ==="
clang --version | head -1

echo "=== 交叉编译到裸机 ELF ==="
clang --target=x86_64-unknown-none-elf -ffreestanding -nostdlib -mno-red-zone \
      -fno-pic -fno-stack-protector -c "$WORK/t.c" -o "$WORK/t_none.o"

file "$WORK/t_none.o"
echo "=== objdump -f ==="
objdump -f "$WORK/t_none.o"
