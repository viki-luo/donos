#!/usr/bin/env bash
# run-trace.sh — 在 QEMU 中启动并把串口输出写入文件
#
# 之所以写文件而不是直接看终端：
# 内核如果跑飞，串口会不断输出垃圾，终端刷屏看不到有用信息。
# 落入文件后可以只看开头几百字节。
set -e
export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH

cd /c/Users/Administrator/Desktop/os
BIN=build/x86_64/liteos.bin
LOG=${1:-build/serial.log}

if [ ! -f "$BIN" ]; then
        echo "请先构建: tools/build.sh"
        exit 1
fi

rm -f "$LOG"

timeout 5 qemu-system-x86_64 \
        -kernel "$BIN" \
        -m 256M \
        -serial "file:$LOG" \
        -display none \
        -no-reboot \
        -no-shutdown \
        2>&1 || true

echo "=== 串口日志前 400 字节（十六进制） ==="
if [ -f "$LOG" ]; then
        echo "大小: $(stat -c %s "$LOG") 字节"
        head -c 400 "$LOG" | od -c | head -30
else
        echo "无日志文件"
fi