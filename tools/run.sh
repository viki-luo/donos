#!/usr/bin/env bash
# run.sh — 在 QEMU 中启动内核（带超时，防止卡死）
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
if [ -d /ucrt64/bin ]; then export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH; fi

BIN=build/x86_64/liteos.bin

if [ ! -f "$BIN" ]; then
        echo "请先构建: tools/build.sh"
        exit 1
fi

# -kernel 走 multiboot 加载路径；串口输出接到 stdio。
# 内核不主动退出，因此 timeout(124) 是正常结束；其他非零视为失败。
set +e
timeout 10 qemu-system-x86_64 \
        -kernel "$BIN" \
        -m 256M \
        -serial stdio \
        -display none \
        -no-reboot \
        -no-shutdown \
        2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] || [ "$rc" -eq 124 ] || { echo "QEMU 异常退出（code=$rc）" >&2; exit 1; }

echo "--- QEMU 退出 ---"
