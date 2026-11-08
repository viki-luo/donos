#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# 工具链冒烟测试：确认能编出 64 位裸机 ELF
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
if [ -d /ucrt64/bin ]; then export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH; fi

WORK="$ROOT/build/smoke"
mkdir -p "$WORK"
rm -f "$WORK/t.c" "$WORK/t.o"

printf 'int f(void) { return 42; }\n' > "$WORK/t.c"
gcc -ffreestanding -nostdlib -m64 -c "$WORK/t.c" -o "$WORK/t.o"

echo "=== file ==="
file "$WORK/t.o"
echo "=== objdump -f ==="
objdump -f "$WORK/t.o"
echo "OK: freestanding 64-bit object produced"
