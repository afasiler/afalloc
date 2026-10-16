# Build, test and flash for the STM32F411RE (Nucleo-F411RE).
#
#   make [APP=blink]        build one application (default: blink)
#   make flash APP=...      write it to the board's flash over SWD
#   make all-apps           build every application (compile + link check)
#   make host-test          run the pool unit tests on this computer (sanitizers on)
#   make emu-test           run the real startup code + pool on an emulated Cortex-M4 (QEMU)
#   make size APP=...       flash / RAM usage        make disasm APP=...   listing
#   make clean
#
# Applications (src/apps/app_<name>.c):
#   blink          LED + serial counter: the first bring-up test
#   selftest       the pool self-test, run on the board
#   bench          pool allocator timed with the cycle counter
#   bench_malloc   the same benchmark on newlib-nano malloc, for comparison
#
# POOL_DEBUG=0|1    the pool's extra checks (default 1; bench builds default 0).
# POOL_IRQ_SAFE=0|1 interrupt masking inside alloc/free (default 1; 0 shows what it costs).
# POOL_TRACK_PEAK=0|1 per-pool peak usage counter (default 1; 0 saves a few cycles per alloc).
# CLOCK_MHZ=96|100  CPU clock from the PLL (default 96: the HSI is only +-1 % accurate, see clock.h).
# Close any running `openocd` before `make flash`: the ST-Link accepts one client.

APP ?= blink
APPS = blink selftest bench bench_malloc

TOOLCHAIN ?= $(HOME)/tools/arm-gnu-expanded/Payload/bin/
CC      = $(TOOLCHAIN)arm-none-eabi-gcc
OBJDUMP = $(TOOLCHAIN)arm-none-eabi-objdump
OBJCOPY = $(TOOLCHAIN)arm-none-eabi-objcopy
SIZE    = $(TOOLCHAIN)arm-none-eabi-size

BUILD = build/$(APP)

COMMON_SRCS = startup.c main.c clock.c board.c uart.c pool.c pool_selftest.c
APP_SRCS    = apps/app_$(APP).c
ifneq (,$(filter bench bench_malloc,$(APP)))
  APP_SRCS += bench_common.c
endif
ifeq ($(APP),bench_malloc)
  APP_SRCS += sbrk.c
endif
# The emulator test has its own entry point and skips the board-specific parts.
ifeq ($(APP),emu_selftest)
  COMMON_SRCS = startup.c uart.c pool.c pool_selftest.c
  APP_SRCS    = emu/emu_main.c
endif
SRCS = $(addprefix src/,$(COMMON_SRCS) $(APP_SRCS))
OBJS = $(SRCS:src/%.c=$(BUILD)/%.o)

# The benchmark builds default to the fastest pool (no debug checks).
ifndef POOL_DEBUG
  ifneq (,$(filter bench bench_malloc,$(APP)))
    POOL_DEBUG = 0
  else
    POOL_DEBUG = 1
  endif
endif

POOL_IRQ_SAFE ?= 1
POOL_TRACK_PEAK ?= 1
CLOCK_MHZ ?= 96

# Cortex-M4 with the single-precision FPU, Thumb instructions.
CPU    = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS = $(CPU) -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding -ffunction-sections -fdata-sections \
         -Isrc -DAPP_NAME=\"$(APP)\" -DPOOL_DEBUG=$(POOL_DEBUG) -DPOOL_IRQ_SAFE=$(POOL_IRQ_SAFE) -DPOOL_TRACK_PEAK=$(POOL_TRACK_PEAK) \
         -DCLOCK_MHZ=$(CLOCK_MHZ) -MMD -MP
# The benchmark: pin function and loop alignment to the flash line size (16 bytes) so code
# placement does not shift the timing by a cycle or two between builds.
ifneq (,$(filter bench bench_malloc,$(APP)))
  CFLAGS += -falign-functions=16 -falign-loops=16
endif
BUILD_INFO = $(APP) -O2 POOL_DEBUG=$(POOL_DEBUG) POOL_IRQ_SAFE=$(POOL_IRQ_SAFE) POOL_TRACK_PEAK=$(POOL_TRACK_PEAK) CLOCK_MHZ=$(CLOCK_MHZ)
CFLAGS += -DBUILD_INFO='"$(BUILD_INFO)"'

# startup.c runs before the FPU is enabled: forbid the compiler from using FP/vector registers there.
$(BUILD)/startup.o: CFLAGS += -mgeneral-regs-only

# We bring our own startup code and linker script. Only the malloc benchmark links the C library.
ifeq ($(APP),bench_malloc)
  LDFLAGS = $(CPU) -nostartfiles --specs=nano.specs --specs=nosys.specs -T linker.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/fw.map
  LDLIBS  = -lc -lgcc
else
  LDFLAGS = $(CPU) -nostartfiles -nostdlib -T linker.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/fw.map
  LDLIBS  = -lgcc
endif

all: $(BUILD)/fw.elf $(BUILD)/fw.bin

# Rebuild objects when a header they include changes (-MMD writes the .d files).
-include $(OBJS:.o=.d)

# Objects also depend on a stamp holding the compile flags, so changing POOL_DEBUG, CLOCK_MHZ, etc.
# rebuilds them instead of silently reusing objects built with other settings.
export FLAGS_TEXT := $(CC) $(CFLAGS)
$(BUILD)/flags.stamp: FORCE | $(BUILD)
	@printf '%s\n' "$$FLAGS_TEXT" | cmp -s - $@ || printf '%s\n' "$$FLAGS_TEXT" > $@
FORCE:

$(BUILD)/%.o: src/%.c $(BUILD)/flags.stamp | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fw.elf: $(OBJS) linker.ld
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

$(BUILD)/fw.bin: $(BUILD)/fw.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD):
	mkdir -p $(BUILD)

flash: $(BUILD)/fw.elf
	openocd -f board/st_nucleo_f4.cfg -c "program $(BUILD)/fw.elf verify reset exit"

size: $(BUILD)/fw.elf
	$(SIZE) $<

disasm: $(BUILD)/fw.elf
	$(OBJDUMP) -d -S $< > $(BUILD)/fw.lst

all-apps:
	@for a in $(APPS); do echo "== $$a"; $(MAKE) --no-print-directory APP=$$a || exit 1; done

# ---- run the real firmware startup + pool on an emulated Cortex-M4 (no board needed) ----
# Two things would hide a missing .bss clear, and both are avoided here:
#  - QEMU's RAM starts zeroed: it is filled with 0xA5 first;
#  - `-kernel fw.elf` makes QEMU itself zero the .bss segment while loading: so the raw flash
#    image (fw.bin) is loaded instead, at 0x08000000, and the CPU boots from its vector table.
# A watchdog (perl alarm) kills QEMU if the program hangs, e.g. stuck in the default handler.
QEMU ?= qemu-system-arm
EMU_TIMEOUT ?= 40
emu-test:
	$(MAKE) --no-print-directory APP=emu_selftest
	@mkdir -p build
	perl -e 'binmode STDOUT; print chr(0xA5) x 131072' > build/ram_fill.bin
	@test "$$(head -c 4 build/ram_fill.bin | xxd -p)" = a5a5a5a5 || { echo 'ram_fill.bin is not 0xA5-filled: the .bss test would prove nothing'; exit 1; }
	perl -e 'alarm shift; exec @ARGV' $(EMU_TIMEOUT) $(QEMU) -machine netduinoplus2 -display none -monitor none \
	        -semihosting-config enable=on,target=native -serial null -serial stdio \
	        -device loader,file=build/ram_fill.bin,addr=0x20000000,force-raw=on \
	        -device loader,file=build/emu_selftest/fw.bin,addr=0x08000000,force-raw=on

# ---- pool unit tests on this computer (no board needed) ----
HOSTCC ?= cc
HOSTFLAGS = -std=c11 -Wall -Wextra -g -O1 -DPOOL_HOST -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc
host-test:
	@mkdir -p build
	$(HOSTCC) $(HOSTFLAGS) host/test_pool_host.c src/pool.c src/pool_selftest.c -o build/test_pool_host
	build/test_pool_host

# White-box tests (they #include pool.c to corrupt its state); run with and without spill.
host-test-white:
	@mkdir -p build
	$(HOSTCC) $(HOSTFLAGS) host/test_pool_whitebox.c -o build/test_pool_whitebox
	build/test_pool_whitebox
	$(HOSTCC) $(HOSTFLAGS) -DPOOL_SPILL=1 host/test_pool_whitebox.c -o build/test_pool_whitebox_spill
	build/test_pool_whitebox_spill
	$(HOSTCC) $(HOSTFLAGS) -DPOOL_DEBUG=0 host/test_pool_whitebox.c -o build/test_pool_whitebox_release
	build/test_pool_whitebox_release
	@# Tables whose pools are NOT a multiple of the owner granule (padding between pools), one pool, big blocks.
	@for cfg in "-D'POOL_TABLE(X)=X(8,3)'" "-D'POOL_TABLE(X)=X(8,3) X(16,5) X(32,7)'" \
	            "-DPOOL_SPILL=1 -D'POOL_TABLE(X)=X(8,3) X(16,5) X(32,7)'" "-DPOOL_DEBUG=0 -D'POOL_TABLE(X)=X(8,9) X(64,3) X(1024,2)'" \
	            "-DPOOL_OWNER_GRANULE=8 -D'POOL_TABLE(X)=X(8,3) X(16,5)'" "-DPOOL_OWNER_GRANULE=256 -D'POOL_TABLE(X)=X(8,3) X(16,5) X(512,2)'" \
	            "-DPOOL_EXPECT_BAD_TABLE -D'POOL_TABLE(X)=X(16,2) X(16,3)'" "-DPOOL_EXPECT_BAD_TABLE -D'POOL_TABLE(X)=X(32,2) X(16,3)'" ; do \
	  printf "%-78s" "[$$cfg]"; \
	  eval $(HOSTCC) $(HOSTFLAGS) $$cfg host/test_pool_whitebox.c -o build/test_pool_whitebox_cfg || exit 1; \
	  build/test_pool_whitebox_cfg | tail -1; \
	done

# The same unit tests under many pool configurations: tiny pools, one pool, big blocks, both debug modes, spill.
host-test-configs:
	@mkdir -p build
	@for cfg in "" "-DPOOL_DEBUG=0" "-DPOOL_SPILL=1" "-DPOOL_DEBUG=0 -DPOOL_SPILL=1" \
	            "-D'POOL_TABLE(X)=X(8,1)'" "-D'POOL_TABLE(X)=X(8,2) X(16,1)'" \
	            "-D'POOL_TABLE(X)=X(8,3)'" "-D'POOL_TABLE(X)=X(8,8) X(16,3) X(32,12)'" \
	            "-D'POOL_TABLE(X)=X(32,100) X(1024,7)'" "-DPOOL_DEBUG=0 -D'POOL_TABLE(X)=X(8,2) X(64,3)'" \
	            "-DPOOL_SPILL=1 -D'POOL_TABLE(X)=X(8,2) X(16,2) X(64,3)'" ; do \
	  printf "%-62s" "[$$cfg]"; \
	  eval $(HOSTCC) $(HOSTFLAGS) $$cfg host/test_pool_host.c src/pool.c src/pool_selftest.c -o build/test_pool_cfg || exit 1; \
	  build/test_pool_cfg | tail -1; \
	done

# Everything that runs on this computer.
host-test-all: host-test host-test-configs host-test-white host-bench-check

# Exercise the benchmark harness on this computer with a fake cycle counter (logic check only).
host-bench-check:
	@mkdir -p build
	$(HOSTCC) $(HOSTFLAGS) host/test_bench_host.c src/bench_common.c src/pool.c -o build/test_bench_host
	build/test_bench_host

# Prove on the compiled benchmark that every timed call is exactly: ldr (counter) / blx / ldr
# (counter), with nothing the compiler could have slipped in between (see TIME_ALLOC in
# src/bench_common.c).
check-bench-windows:
	@mkdir -p build
	$(CC) $(CPU) -std=gnu11 -O2 -ffreestanding -Isrc -DAPP_NAME=\"check\" -DPOOL_DEBUG=0 \
	      -falign-functions=16 -falign-loops=16 -c src/bench_common.c -o build/bench_window_check.o
	python3 host/check_bench_windows.py $(OBJDUMP) build/bench_window_check.o

clean:
	rm -rf build

.PHONY: FORCE check-bench-windows all flash size disasm all-apps host-test host-test-white host-test-configs host-test-all host-bench-check emu-test clean
