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
# POOL_DEBUG=0|1 chooses the pool's extra checks (default 1; bench builds default 0).
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

# Cortex-M4 with the single-precision FPU, Thumb instructions.
CPU    = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS = $(CPU) -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding -ffunction-sections -fdata-sections \
         -Isrc -DAPP_NAME=\"$(APP)\" -DPOOL_DEBUG=$(POOL_DEBUG) -MMD -MP

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

$(BUILD)/%.o: src/%.c | $(BUILD)
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
QEMU ?= qemu-system-arm
emu-test:
	$(MAKE) --no-print-directory APP=emu_selftest
	$(QEMU) -machine netduinoplus2 -display none -monitor none -semihosting-config enable=on,target=native \
	        -serial null -serial stdio -kernel build/emu_selftest/fw.elf

# ---- pool unit tests on this computer (no board needed) ----
HOSTCC ?= cc
HOSTFLAGS = -std=c11 -Wall -Wextra -g -O1 -DPOOL_HOST -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc
host-test:
	@mkdir -p build
	$(HOSTCC) $(HOSTFLAGS) host/test_pool_host.c src/pool.c src/pool_selftest.c -o build/test_pool_host
	build/test_pool_host

# Exercise the benchmark harness on this computer with a fake cycle counter (logic check only).
host-bench-check:
	@mkdir -p build
	$(HOSTCC) $(HOSTFLAGS) host/test_bench_host.c src/bench_common.c src/pool.c -o build/test_bench_host
	build/test_bench_host

clean:
	rm -rf build

.PHONY: all flash size disasm all-apps host-test host-bench-check emu-test clean
