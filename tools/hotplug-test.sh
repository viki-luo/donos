#!/usr/bin/env bash
# hotplug-test.sh — 运行时热插拔验证
#
# 内核启动后，通过 QEMU 监视器在运行中插入一块 e1000 网卡，再把它拔出。
# 期望串口出现一对 [hotplug] arrive / [hotplug] depart。
set -e
export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH

cd /c/Users/Administrator/Desktop/os
BIN=build/x86_64/liteos.bin
LOG=${1:-build/hotplug.log}

if [ ! -f "$BIN" ]; then
        echo "请先构建: tools/build.sh"
        exit 1
fi

rm -f "$LOG"

{
        sleep 3
        echo "device_add e1000,id=hp1,bus=pci.0,addr=0x5"
        sleep 2
        echo "device_del hp1"
        sleep 2
        echo "quit"
} | timeout 15 qemu-system-x86_64 \
        -kernel "$BIN" \
        -m 256M \
        -serial "file:$LOG" \
        -monitor stdio \
        -display none \
        -no-reboot \
        >/dev/null 2>&1 || true

echo "=== 热插拔相关串口输出 ==="
tr -d '\r' < "$LOG" | grep -E 'hotplug|discovered' || echo "(无热插拔事件)"
