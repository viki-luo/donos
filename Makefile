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

# 工具名在不同宿主间不一致：MSYS2 只有 GNU binutils（nm/size），
# Linux CI 装了 llvm 包才有 llvm-nm/llvm-size。优先 llvm-*，缺失时
# 退回 GNU 同名工具，保证两个环境都能跑 make size 与 make check。
# Tool names differ across hosts: MSYS2 has only GNU binutils (nm/size),
# while Linux CI installs the llvm package (llvm-nm/llvm-size). Prefer
# llvm-*, fall back to GNU so both environments work.
NM   := $(shell command -v llvm-nm 2>/dev/null || command -v nm 2>/dev/null || echo nm)
SIZE := $(shell command -v llvm-size 2>/dev/null || command -v size 2>/dev/null || echo size)

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
	$(SIZE) $<

# ---------------------------------------------------------------------
# make check —— 本地跑与 CI 相同的门禁
#
# 存在的理由：如果本地和 CI 跑的不是同一件事，CI 就会变成意外而不是保障。
# 这里检查的是「跨架构一致性」，不是绝对性能数字 —— 后者依赖宿主，
# 写进门禁会把偶发波动变成假失败。
#
# 运行时符号那条断言的是事实而不是猜测：裸机链接没有合成运行时库，
# 所以镜像里任何 undefined 符号都会在运行时跳进空地址。早先写的
# 「变量除数会产生 __udivdi3」在两个目标上都不成立（rv64imac 有 M
# 扩展、x86_64 有 divq），那条检查因此从未触发过，是空跑。真正会
# 拉进 libgcc 的是 __int128 运算缺硬件支持时（例如 rv64iac 上的
# __multi3）以及将来的软浮点。
#
# make check —— the same gate CI runs
#
# Why it exists: if local and CI run different things, CI becomes a surprise
# instead of a guarantee. It asserts cross-architecture consistency, never
# absolute performance — those depend on the host, and asserting them would
# turn jitter into false failures.
#
# The undefined-symbol assertion states a fact rather than a guess: a bare
# link has no synthetic runtime library, so any undefined symbol left in the
# image is a jump to address zero. The earlier rule ("a variable divisor emits
# __udivdi3") is false on both targets — rv64imac has the M extension and
# x86_64 has divq — so it never fired and had silently been a no-op.
# What actually drags libgcc in is __int128 arithmetic without hardware
# support (e.g. __multi3 on rv64iac), and soft float later on.
# ---------------------------------------------------------------------
check:
	@set -e; \
	echo "=== 构建两个架构 / building both architectures ==="; \
	$(MAKE) ARCH=x86_64 >/dev/null; \
	$(MAKE) ARCH=riscv64 >/dev/null; \
	echo "=== 运行测量与压力测试 / running measurements ==="; \
	rm -f build/bench-x86_64.log build/bench-riscv64.log; \
	tools/bench.sh >/dev/null 2>&1; \
	for arch in x86_64 riscv64; do \
		log=build/bench-$$arch.log; \
		test -s $$log || { echo "缺少日志 $$log"; exit 1; }; \
		if grep -qE '^  FAIL ' $$log; then \
			echo "$$arch 自检有明确失败行"; grep -E '^  FAIL ' $$log; exit 1; \
		fi; \
		grep -qE '^  failures = 0$$' $$log || { echo "$$arch 自检失败"; grep -E '^  (FAIL|failures)' $$log; exit 1; }; \
		grep -qE '^  bench failures = 0$$' $$log || { echo "$$arch 压力测试失败"; grep -E 'bench failures' $$log; exit 1; }; \
		grep -q 'protection: what the MMU actually enforces' $$log || { echo "$$arch 未自报保护状态"; exit 1; }; \
		if logs=$$($(NM) build/$$arch/liteos.bin 2>/dev/null | grep ' U ' | awk '{print $$2}'); \
		   [ -n "$$logs" ]; then \
			echo "$$arch 仍有未定义的运行时符号（裸机没有库可链接）:"; \
			echo "$$logs"; exit 1; \
		fi; \
		echo "  ok   $$arch"; \
	done; \
	echo "=== 全部通过 / all checks passed ==="

clean:
	rm -rf build

# ---------------------------------------------------------------------
# 独立门禁：每一项都只断言一个可证伪的事实。
# ---------------------------------------------------------------------

# 符号门禁：镜像必须存在、非空，且不含任何未定义符号。
# 裸机没有运行时库可链接，所以任何 ' U ' 符号都是运行时跳向地址零。
.PHONY: check-symbols check-docs check-release check-all check-purity \
        host-test sanitize
check-symbols:
	@set -e; \
	for arch in x86_64 riscv64; do \
		bin=build/$$arch/liteos.bin; \
		test -s $$bin || { echo "缺少镜像 $$bin"; exit 1; }; \
		out=$$($(NM) $$bin 2>/dev/null | grep ' U ' || true); \
		if [ -n "$$out" ]; then \
			echo "$$arch 仍有未定义符号:"; echo "$$out"; exit 1; \
		fi; \
		echo "  ok   $$arch 无未定义符号"; \
	done

# 文档门禁：中英文必须成对存在，且不得残留发布占位符
check-docs:
	@set -e; \
	for pair in design conformance assessment boot; do \
		test -f docs/$$pair.md || { echo "缺少 docs/$$pair.md"; exit 1; }; \
		test -f docs/$$pair.en.md || { echo "缺少 docs/$$pair.en.md"; exit 1; }; \
	done; \
	echo "  ok   中英文文档成对"; \
	if grep -Rn 'OWNER/REPO\|@OWNER' --include='*.md' --include='*.yml' --include='*.cff' . ; then \
		echo "  仍有 OWNER/REPO 占位符（发布前必须替换）"; exit 1; \
	fi; \
	echo "  ok   无发布占位符"

# 纯净性门禁：基座只做运动控制，因此 base/base.c 里不得出现能力、行程、
# 姿态、质量这些「世界信息」的名字 —— 一旦出现，基座就开始理解世界了，
# 而「基座无状态」这条不变量正是靠「它无从理解」来成立的。
# 这是可证伪的静态断言，不是口号：符号名一旦写进去，门禁立刻失败。
check-purity:
	@set -e; \
	if grep -nE 'cap|limit_min|limit_max|max_rate|mass|orientation|position|described|still' \
		base/base.c; then \
		echo "base/base.c 出现了非运动控制的名字（基座不应理解世界）"; exit 1; \
	fi; \
	echo "  ok   base/base.c 只提到接线与原语"

HOST_CC     ?= clang
HOST_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror \
               -Ibase/include
HOST_TEST   := build/host/test_fixed.exe

$(HOST_TEST): tests/host/test_fixed.c base/fixed.c base/include/fixed.h \
              base/include/base.h base/include/types.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) tests/host/test_fixed.c base/fixed.c -o $@

# 白盒测试：整体包含 base/base.c 才能把接线表推到世代耗尽的边界。
# 四十亿次热插拔不是测试，是等；而边界行为与真跑到底读的是同一张表。
WIRE_TEST := build/host/test_wire.exe

$(WIRE_TEST): tests/host/test_wire.c base/base.c base/include/base.h \
              base/include/types.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) tests/host/test_wire.c -o $@

# 确定性模糊：固定种子 → 可复现。语料覆盖 8 类操作（含专门核对 dof
# 尾部的 apply_tail），不变量由影子表独立对账。
# 注意它抓不到什么：object_plug() 的发布顺序窗口在单线程下是**等价
# 变异体** —— 函数返回前状态已一致，只有并发读者才看得见那个窗口。
# 那条纪律靠顺序论证与复用槽位测试（verify.c 第十三部分）保证，不靠这里。
FUZZ_TEST   := build/host/fuzz_base.exe
FUZZ_ITERS  ?= 200000

$(FUZZ_TEST): tests/host/fuzz_base.c base/base.c base/object.c base/fixed.c \
              base/include/base.h base/include/object.h base/include/fixed.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) tests/host/fuzz_base.c -o $@

fuzz: $(FUZZ_TEST)
	@$(FUZZ_TEST) $(FUZZ_ITERS) | tee build/host/fuzz.log
	@set -e; \
	if grep -q '^  fuzz failures = 0$$' build/host/fuzz.log; then \
		echo "  ok   确定性模糊 0 失败"; \
	else \
		echo "  确定性模糊出现失败"; exit 1; \
	fi

host-test: $(HOST_TEST) $(WIRE_TEST)
	@$(HOST_TEST)
	@$(WIRE_TEST)

# MinGW 的 UBSan 运行库并非所有发行包都完整，因此使用 trap-on-error：
# 编译器在任何 UB 点直接发出陷阱指令，不需要链接 sanitizer runtime。
sanitize:
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_fixed.c base/fixed.c \
		-o build/host/test_fixed_ubsan.exe
	@build/host/test_fixed_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_wire.c \
		-o build/host/test_wire_ubsan.exe
	@build/host/test_wire_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/fuzz_base.c \
		-o build/host/fuzz_base_ubsan.exe
	@build/host/fuzz_base_ubsan.exe 50000

check-release: check-symbols check-docs check-purity
	@echo "=== 发布前门禁通过 / release gate passed ==="

check-all: check host-test fuzz sanitize check-release
	@echo "=== 全部门禁通过 / all gates passed ==="