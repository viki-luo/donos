#!/usr/bin/env bash
# inspect.sh — 检查构建产物
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
if [ -d /ucrt64/bin ]; then export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH; fi

BIN=build/x86_64/liteos.bin

if [ ! -f "$BIN" ]; then
        echo "NOT FOUND: $BIN"
        ls -la build/x86_64/ 2>/dev/null || echo "build/x86_64/ 不存在"
        exit 1
fi

echo "=== file ==="
file "$BIN"
echo "=== ELF header ==="
readelf -h "$BIN"
echo "=== sections ==="
readelf -S "$BIN" | head -20
echo "=== program headers ==="
readelf -l "$BIN"
