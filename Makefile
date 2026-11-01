# liteOS — 构建系统
#
# 工具链：clang + lld（能输出裸机 ELF），nasm，qemu
# 用法：
#   make              # 构建
#   make run          # 在 QEMU 中运行
#   make debug        # 启动 QEMU 等待 gdb
#   make clean

ARCH      ?= x86_64
BUILD     ?= build/$(ARCH)
OUT       := liteos.bin

# ---------------------------------------------------------------------
# 目标三元组：裸机，无宿主 ABI
# ---------------------------------------------------------------------
TARGET_TRIPLE := $(ARCH)-unknown-none-elf

CC        := clang
LD        := clang
AS        := nasm
OBJCOPY   := llvm-objcopy

# ---------------------------------------------------------------------
# 架构相关参数。base/ 的编译选项与架构无关，只有这一段随 ARCH 变化。
# ---------------------------------------------------------------------
ifeq ($(ARCH),x86_64)
ARCH_CFLAGS := -mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx \
               -mgeneral-regs-only
QEMU        := qemu-system-x86_64
QEMUFLAGS   := -m 256M -serial stdio -display none -no-reboot -no-shutdown
else ifeq ($(ARCH),riscv64)
# rv64imac：无浮点扩展，基座本就不碰浮点
ARCH_CFLAGS := -march=rv64imac -mabi=lp64 -mcmodel=medany -mno-relax
QEMU        := qemu-system-riscv64
QEMUFLAGS   := -machine virt -bios default -m 256M -serial stdio \
               -display none -no-reboot
else
$(error 不支持的 ARCH=$(ARCH)，可选 x86_64 / riscv64)
endif

CFLAGS    := --target=$(TARGET_TRIPLE) \
             -std=gnu11 -O2 -g \
             -ffreestanding -fno-stack-protector \
             -fno-pic -fno-pie \
             $(ARCH_CFLAGS) \
             -Wall -Wextra -Werror \
             -Ibase/include -Idrivers/include -Iarch/$(ARCH)/include

# 用 clang 驱动链接，它会按 --target 选择 ELF 模式并使用 lld。
# 直接调 ld.lld 在 MSYS2 上会误用 PE 驱动。
LDFLAGS   := --target=$(TARGET_TRIPLE) -nostdlib -fuse-ld=lld \
             -Wl,-T,arch/$(ARCH)/linker.ld -Wl,--no-rosegment \
             -Wl,-Map,$(BUILD)/kernel.map

ASFLAGS   := -f elf64 -g -F dwarf

# make BOOT_TRACE=1 打开引导阶段单字母标记（A/B/C/D），用于定位卡在哪一步
ifdef BOOT_TRACE
ASFLAGS   += -DBOOT_TRACE
endif

# make FAULT_TEST=1 在自检之后故意触发一次页错误，验证异常处理器。
# 这是唯一一件「故意让内核崩」的开关，默认关闭。
ifdef FAULT_TEST
CFLAGS    += -DFAULT_TEST
endif

# ---------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------
C_SRCS := $(wildcard base/*.c) \
          $(wildcard drivers/*.c) \
          $(wildcard arch/$(ARCH)/*.c)

# x86_64 用 nasm（.asm）；riscv64 用 clang 汇编（.S）
ASM_SRCS := $(wildcard arch/$(ARCH)/*.asm)
S_SRCS   := $(wildcard arch/$(ARCH)/*.S)

C_OBJS   := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
ASM_OBJS := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS))
S_OBJS   := $(patsubst %.S,$(BUILD)/%.o,$(S_SRCS))

OBJS := $(ASM_OBJS) $(S_OBJS) $(C_OBJS)

# ---------------------------------------------------------------------
.PHONY: all run debug clean size dirs check

all: $(BUILD)/$(OUT)

dirs:
	@mkdir -p $(BUILD)/base $(BUILD)/arch/$(ARCH)

$(BUILD)/%.o: %.c | dirs
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm | dirs
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: %.S | dirs
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(OUT): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo "=== 构建完成: $@ ==="

run: $(BUILD)/$(OUT)
	$(QEMU) -kernel $< $(QEMUFLAGS)

debug: $(BUILD)/$(OUT)
	$(QEMU) -kernel $< $(QEMUFLAGS) -s -S

size: $(BUILD)/$(OUT)
	llvm-size $<

# ---------------------------------------------------------------------
# make check —— 本地跑与 CI 相同的门禁
#
# 存在的理由：如果本地和 CI 跑的不是同一件事，CI 就会变成意外而不是保障。
# 这里检查的是「跨架构一致性」与「可链接性」，不是绝对性能数字 ——
# 后者依赖宿主，写进门禁会把偶发波动变成假失败。
#
# make check —— the same gate CI runs
#
# Why it exists: if local and CI run different things, CI becomes a surprise
# instead of a guarantee. It asserts cross-architecture consistency and
# linkability, never absolute performance — those depend on the host, and
# asserting them would turn jitter into false failures.
# ---------------------------------------------------------------------
check:
	@set -e; \
	echo "=== 构建两个架构 / building both architectures ==="; \
	$(MAKE) ARCH=x86_64 >/dev/null; \
	$(MAKE) ARCH=riscv64 >/dev/null; \
	echo "=== 运行测量与压力测试 / running measurements ==="; \
	tools/bench.sh >/dev/null 2>&1 || true; \
	for arch in x86_64 riscv64; do \
		log=build/bench-$$arch.log; \
		test -s $$log || { echo "缺少日志 $$log"; exit 1; }; \
		grep -q 'failures = 0' $$log || { echo "$$arch 自检失败"; exit 1; }; \
		grep -q 'bench failures = 0' $$log || { echo "$$arch 压力测试失败"; exit 1; }; \
		grep -q 'protection: what the MMU actually enforces' $$log || { echo "$$arch 未自报保护状态"; exit 1; }; \
		llvm-nm build/$$arch/liteos.bin 2>/dev/null | grep -Eq '__u?(div|mod)di3' \
			&& { echo "$$arch 出现变量除数运行时符号"; exit 1; } || true; \
		echo "  ok   $$arch"; \
	done; \
	echo "=== 全部通过 / all checks passed ==="

clean:
	rm -rf build