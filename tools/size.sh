#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# size.sh - reproducible image and static-memory budget gate
#
# Uses ELF allocated sections rather than host objcopy output. The installed
# GNU objcopy handles x86_64 but cannot strip the riscv64 ELF, so comparing
# stripped files would silently use two different definitions of "size".
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

if [ -d /ucrt64/bin ]; then
        export PATH=/usr/bin:/bin:/ucrt64/bin:$PATH
fi

# Budgets are deliberately above the measured baseline but close enough to
# catch accidental growth. All values are bytes.
#
# 两套预算：发布镜像（SELFTEST=0）不含任何证据代码，因此限额明显更紧。
# 证据构建保留更松的额度 —— 它是工具，不是产品。
SELFTEST="${SELFTEST:-0}"

if [ "$SELFTEST" = "1" ]; then
        X86_LOAD_MAX=$((224 * 1024))
        X86_STATIC_MAX=$((192 * 1024))
        X86_STACK_MAX=$((64 * 1024))
        RV_LOAD_MAX=$((160 * 1024))
        RV_STATIC_MAX=$((192 * 1024))
        RV_STACK_MAX=$((72 * 1024))
        BUILD_SUBDIR="selftest"
else
        # 发布镜像：证据代码已剥离，超出下面这些数字就说明有东西又混进来了。
        X86_LOAD_MAX=$((160 * 1024))
        X86_STATIC_MAX=$((192 * 1024))
        X86_STACK_MAX=$((64 * 1024))
        RV_LOAD_MAX=$((112 * 1024))
        RV_STATIC_MAX=$((192 * 1024))
        RV_STACK_MAX=$((72 * 1024))
        BUILD_SUBDIR=""
fi

num_section() {
        size -A "$1" | awk -v name="$2" '$1 == name { print $2; found=1 } END { if (!found) print 0 }'
}

sym_hex() {
        nm -n "$1" | awk -v name="$2" '$3 == name { print $1; exit }'
}

hex_to_dec() {
        printf '%d' "$((16#$1))"
}

check_le() {
        local arch="$1" label="$2" value="$3" limit="$4"
        if [ "$value" -le "$limit" ]; then
                printf '  ok   %s %s = %s bytes <= %s\n' "$arch" "$label" "$value" "$limit"
        else
                printf '  FAIL %s %s = %s bytes > %s\n' "$arch" "$label" "$value" "$limit"
                failures=$((failures + 1))
        fi
}

measure() {
        local arch="$1"
        local image="build/$1${BUILD_SUBDIR:+/$BUILD_SUBDIR}/liteos.bin"
        local text rodata data note load bss_start bss_end bss stack_bottom stack_top stack page_tables static_ram

        test -s "$image" || { echo "missing image: $image" >&2; exit 1; }

        text=$(num_section "$image" .text)
        rodata=$(num_section "$image" .rodata)
        data=$(num_section "$image" .data)
        note=$(num_section "$image" .note.xen)
        load=$((text + rodata + data + note))

        bss_start=$(sym_hex "$image" __bss_start)
        bss_end=$(sym_hex "$image" __bss_end)
        bss=$(( $(hex_to_dec "$bss_end") - $(hex_to_dec "$bss_start") ))

        stack_bottom=$(sym_hex "$image" stack_bottom)
        stack_top=$(sym_hex "$image" stack_top)
        if [ -z "$stack_bottom" ]; then
                stack_bottom=$(sym_hex "$image" __stack_bottom)
                stack_top=$(sym_hex "$image" __stack_top)
        fi
        stack=$(( $(hex_to_dec "$stack_top") - $(hex_to_dec "$stack_bottom") ))

        if [ "$arch" = x86_64 ]; then
                # PML4 + PDPT + PD at fixed physical addresses 0x70000..0x72fff.
                page_tables=$((3 * 4096))
                # x86 .data is file-backed and already contains .bss + stack.
                static_ram=$((data + page_tables))
                check_le "$arch" load_image "$load" "$X86_LOAD_MAX"
                check_le "$arch" static_ram "$static_ram" "$X86_STATIC_MAX"
                check_le "$arch" stack_reservation "$stack" "$X86_STACK_MAX"
        else
                # Sv39 tables are linker-allocated inside .bss and therefore
                # already counted in data. The stack follows .data separately.
                page_tables=$((4 * 4096))
                static_ram=$((data + stack))
                check_le "$arch" load_image "$load" "$RV_LOAD_MAX"
                check_le "$arch" static_ram "$static_ram" "$RV_STATIC_MAX"
                check_le "$arch" stack_reservation "$stack" "$RV_STACK_MAX"
        fi

        printf '  size %s: text=%s rodata=%s data=%s bss=%s stack=%s page_tables=%s load=%s static_ram=%s\n' \
               "$arch" "$text" "$rodata" "$data" "$bss" "$stack" "$page_tables" "$load" "$static_ram"
}

failures=0
if [ "$SELFTEST" = "1" ]; then
        echo '=== image and static-memory budgets (SELFTEST=1, evidence build) ==='
else
        echo '=== image and static-memory budgets (release, no evidence code) ==='
fi
measure x86_64
measure riscv64
printf '  size failures = %s\n' "$failures"
exit "$failures"
