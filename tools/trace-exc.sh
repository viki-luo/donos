#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# trace-exc.sh — 用 QEMU 记录 CPU 异常，定位崩溃地址
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
if [ -d /ucrt64/bin ]; then export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH; fi

BIN=build/x86_64/donos.bin

# 内核不主动退出，timeout(124) 是正常结束；其他非零视为失败。
set +e
timeout 5 qemu-system-x86_64 \
        -kernel "$BIN" \
        -m 256M \
        -serial "file:build/serial.log" \
        -display none \
        -no-reboot \
        -no-shutdown \
        -d int,cpu_reset \
        -D build/qemu-exc.log \
        >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] || [ "$rc" -eq 124 ] || { echo "QEMU 异常退出（code=$rc）" >&2; exit 1; }

echo "=== 串口 ==="
cat build/serial.log | tr -d '\r'
echo ""
echo "=== 异常日志（前 40 行）==="
grep -m 40 -iE 'v=|RIP=|error code|page fault|#GP|#PF|check_exception|triple' build/qemu-exc.log 2>/dev/null | head -40
