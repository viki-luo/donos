#!/usr/bin/env bash
export PATH=/ucrt64/bin:$PATH
echo "=== gcc ==="
gcc --version | head -1
echo "=== nasm ==="
nasm -v
echo "=== qemu-system-x86_64 ==="
qemu-system-x86_64 --version | head -1
echo "=== make ==="
make --version | head -1
echo "=== objcopy ==="
objcopy --version | head -1
echo "=== ld ==="
ld --version | head -1
echo "=== gdb ==="
gdb --version | head -1