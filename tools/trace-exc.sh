#!/usr/bin/env bash
# trace-exc.sh — 用 QEMU 记录 CPU 异常，定位崩溃地址
set -euo pipefail
export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH

cd /c/Users/Administrator/Desktop/os
BIN=build/x86_64/liteos.bin

timeout 5 qemu-system-x86_64 \
        -kernel "$BIN" \
        -m 256M \
        -serial "file:build/serial.log" \
        -display none \
        -no-reboot \
        -no-shutdown \
        -d int,cpu_reset \
        -D build/qemu-exc.log \
        2>/dev/null || true

echo "=== 串口 ==="
cat build/serial.log | tr -d '\r'
echo ""
echo "=== 异常日志（前 40 行）==="
grep -m 40 -iE 'v=|RIP=|error code|page fault|#GP|#PF|check_exception|triple' build/qemu-exc.log 2>/dev/null | head -40