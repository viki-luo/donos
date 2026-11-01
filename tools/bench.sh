#!/usr/bin/env bash
# bench.sh — 在两个架构上跑同一套测量，并把关键数字并排列出
#
# 为什么要有这个脚本：
#   bench.c 的数字只有在「同一次运行内」自比才有意义。手工跑很容易
#   把两次运行的数字放在一起比较，那样得到的结论是错的。这个脚本
#   把两种架构各自的完整日志留下来，并把可比较的量（比值）抽出来。
#
# 用法:
#   tools/bench.sh            # 两个架构都跑
#   tools/bench.sh riscv64    # 只跑一个
set -e
export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH

cd /c/Users/Administrator/Desktop/os
ARCHES="${1:-x86_64 riscv64}"

run_x86_64() {
        echo "--- x86_64 (TSC 计数) ---"
        timeout 6 qemu-system-x86_64 \
                -kernel build/x86_64/liteos.bin \
                -m 256M \
                -serial file:build/bench-x86_64.log \
                -display none -no-reboot -no-shutdown >/dev/null 2>&1 || true
        tr -d '\r' < build/bench-x86_64.log | grep -E \
                'free object|ticks/call|ticks/run|tick period|share|capacity =|churn cycles|bench failures|failures ='
}

run_riscv64() {
        echo "--- riscv64 (time CSR 计数, 10 MHz) ---"
        timeout 25 qemu-system-riscv64 \
                -machine virt -bios default -m 256M \
                -kernel build/riscv64/liteos.bin \
                -device e1000 \
                -serial file:build/bench-riscv64.log \
                -display none -no-reboot >/dev/null 2>&1 || true
        tr -d '\r' < build/bench-riscv64.log | grep -E \
                'free object|ticks/call|ticks/run|tick period|share|capacity =|churn cycles|bench failures|failures ='
}

for a in $ARCHES; do
        case "$a" in
                x86_64)  [ -f build/x86_64/liteos.bin ]  || { echo "缺少 x86_64 镜像，先 make ARCH=x86_64";  exit 1; }
                         run_x86_64 ;;
                riscv64) [ -f build/riscv64/liteos.bin ] || { echo "缺少 riscv64 镜像，先 make ARCH=riscv64"; exit 1; }
                         run_riscv64 ;;
                *)       echo "未知架构: $a"; exit 1 ;;
        esac
done

cat <<'NOTE'

读这些数字时请注意三条：
  1. 只有「同一次运行内的比值」可以当结论，例如
     worst run share of a tick —— 它不依赖计数器刻度。
  2. 每次调用的计数值在虚拟机里是宿主时间，不是芯片时间；
     两者不可跨架构直接比绝对值。
  3. riscv64 的计数器是 10 MHz（一格 100 ns），单次调用精度不足，
     所以那里只测成批调用的平均值。
NOTE
