# SPDX-License-Identifier: GPL-2.0-only
# DONos — 构建系统
#
# 工具链：clang + lld（能输出裸机 ELF），nasm，qemu
# 用法：
#   make              # 构建
#   make run          # 在 QEMU 中运行
#   make debug        # 启动 QEMU 等待 gdb
#   make clean

ARCH      ?= x86_64
# SELFTEST=1：把自检与基准编进镜像（用于验证）；默认 0 构建发布镜像。
# 两者输出到不同目录，互不覆盖，因此 make check 与发布构建可以先后共存。
SELFTEST  ?= 0
BUILD     ?= build/$(ARCH)$(if $(filter 1,$(SELFTEST)),/selftest,)
OUT       := donos.bin

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
             -Ibase/include -Ibrain/include -Imonitor/include -Idrivers/include -Iarch/$(ARCH)/include

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
#
# 证据代码（自检、基准、故障探针）只在 SELFTEST=1 时编入。
# 理由：自检是证明手段，不是产品的一部分。把它无条件链进发布镜像，
# 会让每个部署都背着几十 KB 的测试代码 —— 而微内核的价值恰恰是「小」。
# ---------------------------------------------------------------------
# 每个架构的「仅自检」文件不同：riscv64 的 mmu.c 是真正的建表代码
# （无条件需要），而 x86_64 的 mmu.c 只是探测辅助。
ifeq ($(ARCH),x86_64)
SELFTEST_ONLY_C := base/verify.c base/bench.c brain/brain_verify.c \
                   monitor/monitor_verify.c \
                   arch/x86_64/mmu.c arch/x86_64/mmu_verify.c
SELFTEST_ONLY_PROBE := arch/x86_64/mmu_probe.asm
else
SELFTEST_ONLY_C := base/verify.c base/bench.c brain/brain_verify.c \
                   monitor/monitor_verify.c arch/riscv64/mmu_verify.c
SELFTEST_ONLY_PROBE := arch/riscv64/mmu_probe.S
endif

C_SRCS := $(filter-out $(SELFTEST_ONLY_C), \
            $(wildcard base/*.c) \
            $(wildcard brain/*.c) \
            $(wildcard monitor/*.c) \
            $(wildcard drivers/*.c) \
            $(wildcard arch/$(ARCH)/*.c))

# x86_64 用 nasm（.asm）；riscv64 用 clang 汇编（.S）
ASM_SRCS := $(filter-out arch/$(ARCH)/mmu_probe.asm, \
              $(wildcard arch/$(ARCH)/*.asm))
S_SRCS   := $(filter-out arch/$(ARCH)/mmu_probe.S, \
              $(wildcard arch/$(ARCH)/*.S))

# 故障注入探针只在自检构建里链接：它们的存在本身就是为了故意违约。
ifeq ($(SELFTEST),1)
CFLAGS   += -DDONOS_SELFTEST=1
C_SRCS   += $(SELFTEST_ONLY_C)
ifneq ($(filter %.asm,$(SELFTEST_ONLY_PROBE)),)
ASM_SRCS += $(SELFTEST_ONLY_PROBE)
else
S_SRCS   += $(SELFTEST_ONLY_PROBE)
endif
endif

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
bench-latency:
	@set -e; \
	$(MAKE) ARCH=x86_64 SELFTEST=1 >/dev/null; \
	$(MAKE) ARCH=riscv64 SELFTEST=1 >/dev/null; \
	rm -f build/bench-x86_64.log build/bench-riscv64.log; \
	SELFTEST=1 tools/bench.sh >/dev/null; \
	for arch in x86_64 riscv64; do \
		log=build/bench-$$arch.log; \
		grep -q '^  latency p50     :' $$log || { echo "$$arch 缺少 p50"; exit 1; }; \
		grep -q '^  latency p99     :' $$log || { echo "$$arch 缺少 p99"; exit 1; }; \
		grep -q '^  latency p99.9   :' $$log || { echo "$$arch 缺少 p99.9"; exit 1; }; \
		grep -q '^  latency max     :' $$log || { echo "$$arch 缺少 max"; exit 1; }; \
		grep -q '^  latency histogram ' $$log || { echo "$$arch 缺少直方图"; exit 1; }; \
		grep -q '^  ok   latency budget: p99.9 is below 50% of one tick$$' $$log || { echo "$$arch p99.9 超预算"; exit 1; }; \
		grep -q '^  ok   latency budget: max is below 75% of one tick$$' $$log || { echo "$$arch max 超预算"; exit 1; }; \
		echo "  ok   $$arch latency distribution"; \
	done

check:
	@set -e; \
	echo "=== 构建两个架构 / building both architectures ==="; \
	$(MAKE) ARCH=x86_64 SELFTEST=1 >/dev/null; \
	$(MAKE) ARCH=riscv64 SELFTEST=1 >/dev/null; \
	echo "=== 运行测量与压力测试 / running measurements ==="; \
	rm -f build/bench-x86_64.log build/bench-riscv64.log; \
	SELFTEST=1 tools/bench.sh >/dev/null 2>&1; \
	for arch in x86_64 riscv64; do \
		log=build/bench-$$arch.log; \
		test -s $$log || { echo "缺少日志 $$log"; exit 1; }; \
		if grep -qE '^  FAIL ' $$log; then \
			echo "$$arch 自检有明确失败行"; grep -E '^  FAIL ' $$log; exit 1; \
		fi; \
		grep -qE '^  failures = 0$$' $$log || { echo "$$arch 自检失败"; grep -E '^  (FAIL|failures)' $$log; exit 1; }; \
		grep -qE '^  bench failures = 0$$' $$log || { echo "$$arch 压力测试失败"; grep -E 'bench failures' $$log; exit 1; }; \
		grep -q '^  endurance ticks = ' $$log || { echo "$$arch 缺少长期运行证据"; exit 1; }; \
		grep -q '^  storm ticks = ' $$log || { echo "$$arch 缺少中断风暴证据"; exit 1; }; \
		if [ "$$arch" = riscv64 ]; then \
			grep -q '^  ok   \.data initializer survived boot$$' $$log || { echo "riscv64 静态初值在引导清零中丢失"; exit 1; }; \
		fi; \
		grep -qE '^  monitor failures = 0$$' $$log || { echo "$$arch 监视器自检失败"; grep -E 'monitor failures' $$log; exit 1; }; \
		grep -qE '^  brain failures = 0$$' $$log || { echo "$$arch 大脑热切换自检失败"; grep -E 'brain failures' $$log; exit 1; }; \
		grep -qE '^  mmu failures = 0$$' $$log || { echo "$$arch MMU 自检失败"; grep -E 'mmu failures' $$log; exit 1; }; \
		grep -q 'protection: what the MMU actually enforces' $$log || { echo "$$arch 未自报保护状态"; exit 1; }; \
		if logs=$$($(NM) build/$$arch/selftest/donos.bin 2>/dev/null | grep ' U ' | awk '{print $$2}'); \
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
        check-single-core check-size check-slim bench-latency fuzz \
        host-test sanitize
check-symbols:
	@set -e; \
	for arch in x86_64 riscv64; do \
		bin=build/$$arch/donos.bin; \
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
# 这条可被证伪的静态断言，不是口号：符号名一旦写进去，门禁立刻失败。
#
# brain 也在名单里（设计决定 13）：基座若能说出「大脑」这个词，
# 它就有了「谁在思考」这个概念，也就有了特权层的雏形。大脑只能是
# 基座之外的一段代码，靠普通接线与基座发生关系。
check-purity:
	@set -e; \
	if grep -nE 'cap|limit_min|limit_max|max_rate|mass|orientation|position|described|still|brain' \
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
WIRE_TEST    := build/host/test_wire.exe
MONITOR_TEST := build/host/test_monitor.exe

$(WIRE_TEST): tests/host/test_wire.c base/base.c base/include/base.h \
              base/include/types.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) tests/host/test_wire.c -o $@

$(MONITOR_TEST): tests/host/test_monitor.c monitor/monitor.c base/quat.c \
                 monitor/include/monitor.h base/base.c base/object.c base/fixed.c \
                 base/include/base.h base/include/object.h base/include/fixed.h \
                 base/include/quat.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) -Imonitor/include tests/host/test_monitor.c base/quat.c -o $@

# 姿态积分与行程限位的宿主边界：期望值全部手算，不回读实现。
QUAT_TEST := build/host/test_quat.exe

# 真实设备挂接：证明实测来自设备而不是命令的副本。
DEVICE_TEST := build/host/test_device.exe

# 车轮适配器：轮径/减速比/编码器换算只在适配器里，物件侧仍是 m/s。
WHEEL_TEST := build/host/test_wheel.exe

# 关节适配器与串联链：一条接线一个关节，链条由参照系表达。
JOINT_TEST := build/host/test_joint.exe

# 定点正余弦：关节角写成姿态四元数需要它。
SINCOS_TEST := build/host/test_sincos.exe

$(SINCOS_TEST): tests/host/test_sincos.c base/fixed.c
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) tests/host/test_sincos.c -o $@

# 控制回路：只读实测、只发原语。
CONTROL_TEST := build/host/test_control.exe

# 控制律热插拔：换命令来源，不搬状态。
HANDOVER_TEST := build/host/test_handover.exe

# 多关节协调：一条腿是两条接线，足端位置由大脑用实测角度算出。
GAIT_TEST := build/host/test_gait.exe

# 逆运动学与足端轨迹：说「足端去这里」，系统到达那里。
FOOT_TEST := build/host/test_foot.exe

# 连续足端轨迹：目标在动，误差界由 v/K 推出。
TRACK_TEST := build/host/test_track.exe

$(TRACK_TEST): tests/host/test_track.c brain/trajectory.c brain/kinematics.c \
               brain/control.c brain/handover.c brain/include/trajectory.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) -Ibrain/include tests/host/test_track.c -o $@

$(FOOT_TEST): tests/host/test_foot.c brain/kinematics.c brain/control.c \
              brain/handover.c brain/include/kinematics.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) -Ibrain/include tests/host/test_foot.c -o $@

$(GAIT_TEST): tests/host/test_gait.c brain/gait.c brain/kinematics.c \
              brain/handover.c brain/include/gait.h brain/include/kinematics.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) -Ibrain/include tests/host/test_gait.c -o $@

$(HANDOVER_TEST): tests/host/test_handover.c brain/handover.c \
                  brain/include/handover.h brain/include/control.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) -Ibrain/include tests/host/test_handover.c -o $@

$(CONTROL_TEST): tests/host/test_control.c brain/control.c \
                 brain/include/control.h base/include/motion_device.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) -Ibrain/include tests/host/test_control.c -o $@

# DONOS-MO-1 一致性套件：同一个设备无关套件必须让不同实现都通过。
MO_CONF := build/host/conformance_mo.exe

$(MO_CONF): tests/host/conformance_mo.c drivers/motion/mo_standard.c \
            drivers/include/mo_standard.h drivers/include/wheel_adapter.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) -Idrivers/include tests/host/conformance_mo.c \
		drivers/motion/mo_standard.c -o $@

$(JOINT_TEST): tests/host/test_joint.c drivers/motion/joint_adapter.c \
               drivers/include/joint_adapter.h base/include/motion_device.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) -Idrivers/include tests/host/test_joint.c -o $@

$(WHEEL_TEST): tests/host/test_wheel.c drivers/motion/wheel_adapter.c \
               drivers/include/wheel_adapter.h base/include/motion_device.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) -Idrivers/include tests/host/test_wheel.c -o $@

$(DEVICE_TEST): tests/host/test_device.c base/quat.c base/base.c base/object.c \
                base/fixed.c monitor/monitor.c base/include/base.h \
                base/include/object.h base/include/fixed.h base/include/quat.h \
                base/include/motion_device.h monitor/include/monitor.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) -Imonitor/include tests/host/test_device.c -o $@

$(QUAT_TEST): tests/host/test_quat.c base/base.c base/fixed.c base/quat.c \
              base/object.c base/include/base.h base/include/types.h \
              base/include/fixed.h base/include/quat.h base/include/object.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) tests/host/test_quat.c -o $@

# 确定性模糊：固定种子 → 可复现。语料覆盖 8 类操作（含专门核对 dof
# 尾部的 apply_tail），不变量由影子表独立对账。
# 注意它抓不到什么：object_plug() 的发布顺序窗口在单线程下是**等价
# 变异体** —— 函数返回前状态已一致，只有并发读者才看得见那个窗口。
# 那条纪律靠顺序论证与复用槽位测试（verify.c 第十三部分）保证，不靠这里。
FUZZ_TEST   := build/host/fuzz_base.exe
FUZZ_ITERS  ?= 200000

$(FUZZ_TEST): tests/host/fuzz_base.c base/quat.c base/base.c base/object.c base/fixed.c \
              base/include/base.h base/include/object.h base/include/fixed.h base/include/quat.h
	@mkdir -p build/host
	$(HOST_CC) $(HOST_CFLAGS) tests/host/fuzz_base.c base/quat.c -o $@

fuzz: $(FUZZ_TEST)
	@$(FUZZ_TEST) $(FUZZ_ITERS) | tee build/host/fuzz.log
	@set -e; \
	if grep -q '^  fuzz failures = 0$$' build/host/fuzz.log; then \
		echo "  ok   确定性模糊 0 失败"; \
	else \
		echo "  确定性模糊出现失败"; exit 1; \
	fi

host-test: $(HOST_TEST) $(WIRE_TEST) $(MONITOR_TEST) $(QUAT_TEST) $(DEVICE_TEST) $(WHEEL_TEST) $(JOINT_TEST) $(MO_CONF) $(SINCOS_TEST) $(CONTROL_TEST) $(HANDOVER_TEST) $(GAIT_TEST) $(FOOT_TEST) $(TRACK_TEST)
	@$(HOST_TEST)
	@$(WIRE_TEST)
	@$(MONITOR_TEST)
	@$(QUAT_TEST)
	@$(DEVICE_TEST)
	@$(WHEEL_TEST)
	@$(JOINT_TEST)
	@$(MO_CONF)
	@$(SINCOS_TEST)
	@$(CONTROL_TEST)
	@$(HANDOVER_TEST)
	@$(GAIT_TEST)
	@$(FOOT_TEST)
	@$(TRACK_TEST)

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
	$(HOST_CC) $(HOST_CFLAGS) -Imonitor/include -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_monitor.c base/quat.c \
		-o build/host/test_monitor_ubsan.exe
	@build/host/test_monitor_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_quat.c \
		-o build/host/test_quat_ubsan.exe
	@build/host/test_quat_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -Imonitor/include -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_device.c \
		-o build/host/test_device_ubsan.exe
	@build/host/test_device_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -Imonitor/include -Idrivers/include -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_wheel.c \
		-o build/host/test_wheel_ubsan.exe
	@build/host/test_wheel_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -Imonitor/include -Idrivers/include -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_joint.c \
		-o build/host/test_joint_ubsan.exe
	@build/host/test_joint_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -Idrivers/include -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/conformance_mo.c \
		-o build/host/conformance_mo_ubsan.exe
	@build/host/conformance_mo_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_sincos.c \
		-o build/host/test_sincos_ubsan.exe
	@build/host/test_sincos_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -Ibrain/include -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_control.c \
		-o build/host/test_control_ubsan.exe
	@build/host/test_control_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -Ibrain/include -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_handover.c \
		-o build/host/test_handover_ubsan.exe
	@build/host/test_handover_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -Ibrain/include -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_gait.c \
		-o build/host/test_gait_ubsan.exe
	@build/host/test_gait_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -Ibrain/include -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_foot.c \
		-o build/host/test_foot_ubsan.exe
	@build/host/test_foot_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -Ibrain/include -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/test_track.c \
		-o build/host/test_track_ubsan.exe
	@build/host/test_track_ubsan.exe
	$(HOST_CC) $(HOST_CFLAGS) -fsanitize=undefined \
		-fsanitize-undefined-trap-on-error \
		tests/host/fuzz_base.c base/quat.c \
		-o build/host/fuzz_base_ubsan.exe
	@build/host/fuzz_base_ubsan.exe 50000

check-size:
	@set -e; \
	$(MAKE) ARCH=x86_64 SELFTEST=1 >/dev/null; \
	$(MAKE) ARCH=riscv64 SELFTEST=1 >/dev/null; \
	SELFTEST=1 tools/size.sh | tee build/size.log; \
	grep -q '^  size failures = 0$$' build/size.log || { echo "自检镜像体积或静态内存超出预算"; exit 1; }

# 发布镜像门禁：证据代码必须不在里面，且镜像真的能跑。
#
# 这条门禁存在的理由很具体：自检曾被无条件链进内核，于是每次部署都
# 背着几十 KB 测试代码，而 `make check` 全程绿灯。现在断言三件事：
#   (a) 发布镜像里不存在任何自检/基准/故障探针符号；
#   (b) 发布镜像体积落在更紧的预算内；
#   (c) 发布镜像能启动、跑完 live 演示并报告「物件仍在运动」。
check-slim:
	@set -e; \
	$(MAKE) ARCH=x86_64 SELFTEST=0 >/dev/null; \
	$(MAKE) ARCH=riscv64 SELFTEST=0 >/dev/null; \
	for arch in x86_64 riscv64; do \
		bin=build/$$arch/donos.bin; \
		test -s $$bin || { echo "缺少发布镜像 $$bin"; exit 1; }; \
		if hits=$$($(NM) $$bin 2>/dev/null | grep -E ' (base_verify|bench_run|monitor_verify|brain_verify|arch_mmu_verify)$$'); then \
			echo "$$arch 发布镜像残留自检符号（证据不应随固件出货）:"; \
			echo "$$hits"; exit 1; \
		fi; \
	done; \
	SELFTEST=0 tools/size.sh | tee build/size-release.log; \
	grep -q '^  size failures = 0$$' build/size-release.log || { echo "发布镜像体积超出预算"; exit 1; }; \
	for arch in x86_64 riscv64; do \
		log=build/release-$$arch.log; \
		rm -f $$log; \
		if [ "$$arch" = x86_64 ]; then \
			timeout 40 qemu-system-x86_64 -icount shift=0,align=off,sleep=off \
				-kernel build/x86_64/donos.bin -m 256M \
				-serial file:$$log -display none -no-reboot -no-shutdown >/dev/null 2>&1 || true; \
		else \
			timeout 70 qemu-system-riscv64 -icount shift=0,align=off,sleep=off \
				-machine virt -bios default -m 256M \
				-kernel build/riscv64/donos.bin -device e1000 \
				-serial file:$$log -display none -no-reboot >/dev/null 2>&1 || true; \
		fi; \
		tr -d '\r' < $$log | grep -q 'objects keep moving' || { \
			echo "$$arch 发布镜像没有跑到 live 演示"; tail -20 $$log; exit 1; }; \
		echo "  ok   $$arch release image boots and runs"; \
	done

# 单核门禁：把「目前只支持单核」从形容词变成可证伪的事实。
#   (a) 源码里不得出现并发原语 —— 出现即说明有人开始做 SMP，而表还没有锁；
#   (b) 中断入口不得执行运动环 —— 否则主循环与 ISR 会在无锁表上重入；
#   (c) 设计文档必须同时存在中英文的并发模型章节，声明不能只活在代码注释里。
# 许可证门禁：每个源文件都必须带 SPDX 标识，且与 LICENSE 一致。
#
# SPDX 头是机器可读的许可证声明。没有它，代码一旦被拆走一个文件，
# 那个文件就没有任何许可证信息 —— 这在实际分发里是真实的法律风险。
# 因此这里逐文件断言，而不是只在 README 里写一句。
check-spdx:
	@set -e; \
	if grep -q 'SPDX-License-Identifier' LICENSE && \
	   ! grep -q 'GPL-2.0-only' LICENSE; then \
		echo "LICENSE 里的 SPDX 标识不是 GPL-2.0-only"; exit 1; \
	fi; \
	missing=$$(find . -path ./build -prune -o -path ./.git -prune -o -type f \
		\( -name '*.c' -o -name '*.h' -o -name '*.asm' -o -name '*.S' \
		   -o -name '*.ld' -o -name '*.sh' -o -name 'Makefile' \) -print \
		| xargs grep -L 'SPDX-License-Identifier: GPL-2.0-only' 2>/dev/null || true); \
	if [ -n "$$missing" ]; then \
		echo "以下源文件缺少 SPDX-License-Identifier: GPL-2.0-only"; \
		echo "$$missing"; exit 1; \
	fi; \
	echo "  ok   所有源文件都带 GPL-2.0-only 的 SPDX 标识"

check-single-core:
	@set -e; \
	if hits=$$(grep -rInE '__atomic|_Atomic|spinlock|spin_lock|mutex|memory_order|__sync_' \
		base brain monitor drivers arch base/include 2>/dev/null); then \
		echo "源码出现并发原语，但接线表与物件表尚未加锁（见 docs/design.md 并发模型）:"; \
		echo "$$hits"; exit 1; \
	fi; \
	for f in arch/x86_64/idt.c arch/x86_64/isr.asm arch/riscv64/trap.c arch/riscv64/traps.S; do \
		if hits=$$(grep -nE 'object_run|base_apply|object_step' $$f); then \
			echo "$$f 在中断路径里执行运动环（会造成无锁重入）:"; \
			echo "$$hits"; exit 1; \
		fi; \
	done; \
	for d in docs/design.md docs/design.en.md; do \
		grep -qE '^## (9|10)\. (并发模型|Concurrency model)' $$d || { \
			echo "$$d 缺少并发模型章节"; exit 1; }; \
	done; \
	echo "  ok   单核、不可重入：无并发原语，中断不跑运动环，模型已声明"

check-release: check-symbols check-docs check-purity check-single-core check-spdx
	@echo "=== 发布前门禁通过 / release gate passed ==="

check-all: check bench-latency check-size check-slim host-test fuzz \
           sanitize check-release
	@echo "=== 全部门禁通过 / all gates passed ==="