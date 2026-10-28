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
.PHONY: all run debug clean size dirs

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

clean:
	rm -rf build