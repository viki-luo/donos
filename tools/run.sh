#!/usr/bin/env bash
# run.sh — 在 QEMU 中启动内核（带超时，防止卡死）
set -e
export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH

cd /c/Users/Administrator/Desktop/os
BIN=build/x86_64/liteos.bin

if [ ! -f "$BIN" ]; then
        echo "请先构建: tools/build.sh"
        exit 1
fi

# -kernel 走 multiboot 加载路径；串口输出接到 stdio
timeout 10 qemu-system-x86_64 \
        -kernel "$BIN" \
        -m 256M \
        -serial stdio \
        -display none \
        -no-reboot \
        -no-shutdown \
        2>&1 || true

echo "--- QEMU 退出 ---"