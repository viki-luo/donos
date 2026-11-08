#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
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
set -euo pipefail

# 定位仓库根，不再依赖任何写死的开发机路径。
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

# MSYS2 基础环境需要显式补全 PATH（Linux/macOS 上目录不存在，忽略）
if [ -d /ucrt64/bin ]; then
        export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH
fi

ARCHES="${1:-x86_64 riscv64}"

# 证据构建与发布构建输出到不同目录：自检/基准跑的是 SELFTEST=1 那份，
# 因为它才含 bench 代码；发布镜像不含任何证据代码。
SELFTEST="${SELFTEST:-1}"
if [ "$SELFTEST" = "1" ]; then
        IMG_SUBDIR="selftest"
else
        IMG_SUBDIR=""
fi

img_path() {
        if [ -n "$IMG_SUBDIR" ]; then
                printf 'build/%s/%s/liteos.bin' "$1" "$IMG_SUBDIR"
        else
                printf 'build/%s/liteos.bin' "$1"
        fi
}

# 运行 QEMU：内核不主动退出，因此 timeout(124) 是正常结束；
# 其他非零退出（镜像错误、启动即崩）视为失败。
# Run QEMU: the kernel never exits on its own, so timeout (124) is the normal
# end; any other non-zero exit (bad image, early crash) is a failure.
run_qemu() {
        local log="$1"; shift
        set +e
        timeout "$@" >/dev/null 2>&1
        local rc=$?
        set -e
        if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
                echo "QEMU 异常退出（code=$rc），不是正常的超时结束" >&2
                exit 1
        fi
        # 日志必须包含压力测试的完成标记，否则视为没有跑完。
        # The log must carry the stress-test completion marker.
        tr -d '\r' < "$log" | grep -q 'bench failures =' || {
                echo "缺少完成标记 bench failures = 于 $log" >&2
                exit 1
        }
}

run_x86_64() {
        echo "--- x86_64 (TSC 计数) ---"
        run_qemu build/bench-x86_64.log 30 \
                qemu-system-x86_64 \
                -icount shift=0,align=off,sleep=off \
                -kernel "$(img_path x86_64)" \
                -m 256M \
                -serial file:build/bench-x86_64.log \
                -display none -no-reboot -no-shutdown
        tr -d '\r' < build/bench-x86_64.log | grep -E \
                'free object|ticks/call|latency (average|p50|p99 |p99.9|max)|histogram|bucket|tick period|run share|latency budget|capacity =|churn cycles|bench failures|failures =|endurance ticks|storm ticks'
}

run_riscv64() {
        echo "--- riscv64 (time CSR 计数, 10 MHz) ---"
        run_qemu build/bench-riscv64.log 60 \
                qemu-system-riscv64 \
                -icount shift=0,align=off,sleep=off \
                -machine virt -bios default -m 256M \
                -kernel "$(img_path riscv64)" \
                -device e1000 \
                -serial file:build/bench-riscv64.log \
                -display none -no-reboot
        tr -d '\r' < build/bench-riscv64.log | grep -E \
                'free object|ticks/call|latency (average|p50|p99 |p99.9|max)|histogram|bucket|tick period|run share|latency budget|capacity =|churn cycles|bench failures|failures =|endurance ticks|storm ticks'
}

for a in $ARCHES; do
        case "$a" in
                x86_64)  [ -f "$(img_path x86_64)" ]  || { echo "缺少 x86_64 镜像，先 make ARCH=x86_64 SELFTEST=$SELFTEST";  exit 1; }
                         run_x86_64 ;;
                riscv64) [ -f "$(img_path riscv64)" ] || { echo "缺少 riscv64 镜像，先 make ARCH=riscv64 SELFTEST=$SELFTEST"; exit 1; }
                         run_riscv64 ;;
                *)       echo "未知架构: $a"; exit 1 ;;
        esac
done

cat <<'NOTE'

读这些数字时请注意三条：
  1. 只有同一次运行、同一计数器内的比值可以作为门禁结论，例如
     p99.9 run share of a tick；绝对计数不可跨架构比较。
  2. 普通 QEMU 运行会混入宿主调度暂停，曾测出超过一个节拍的虚假尾值；
     门禁使用 -icount shift=0,align=off,sleep=off 固定指令推进，形成可重复基线。
  3. icount 不是物理芯片时间。真实硬件仍需另跑同一分位数与预算，
     不能把这里的计数值冒充真实纳秒或 WCET 证明。
NOTE
