#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# build.sh — 在 MSYS2 环境下构建 DONos
#
# 之所以需要这个脚本而不是直接 make：
# 非登录 shell 下 MSYS2 的 PATH 不完整，必须显式补全。
set -e

export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH

cd "$(dirname "$0")/.."
exec make "$@"