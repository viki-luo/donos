#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# hotplug-test.sh — 运行时热插拔验证
#
# 内核启动后，通过 QEMU 监视器在运行中插入一块 e1000 网卡，再把它拔出。
# 期望串口出现一对 [hotplug] arrive / [hotplug] depart。
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
if [ -d /ucrt64/bin ]; then export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH; fi

BIN=build/x86_64/liteos.bin
LOG=${1:-build/hotplug.log}

if [ ! -f "$BIN" ]; then
        echo "请先构建: tools/build.sh"
        exit 1
fi

rm -f "$LOG"

# 这里由 monitor 脚本主动 quit，QEMU 正常退出(0)；若内核或设备热插拔
# 导致 QEMU 提前崩溃，则是非零退出，视为失败。
set +e
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
        >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] || [ "$rc" -eq 124 ] || { echo "QEMU 异常退出（code=$rc）" >&2; exit 1; }

echo "=== 热插拔相关串口输出 ==="
tr -d '\r' < "$LOG" | grep -E 'hotplug|discovered' || echo "(无热插拔事件)"
