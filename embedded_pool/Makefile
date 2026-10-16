# Portable pool allocator: tests that run on this computer, and compile checks for other cores.
#
#   make test-all       everything below that runs here (needs only a C compiler; no board)
#   make test           unit tests (12 tests) under the address and undefined-behaviour sanitizers
#   make test-white     white-box tests (they #include pool.c and corrupt its private state)
#   make test-configs   the unit tests under many pool tables and option combinations
#   make test-bench     the benchmark harness with a fake cycle counter (logic check only)
#   make check-ports    compile the library for Cortex-M0+/M3/M4/M7 (needs arm-none-eabi-gcc)
#   make test-qemu      RUN the self-test on emulated Cortex-M0, M3, M4 and M7 (needs QEMU)
#   make mutate         inject deliberate bugs into pool.c and see whether the tests notice
#
# The firmware project that uses this library on a real board is ../stm32.

HOSTCC ?= cc
INC = -Iinclude
HOSTFLAGS = -std=c11 -Wall -Wextra -g -O1 -DPOOL_HOST -fsanitize=address,undefined -fno-omit-frame-pointer $(INC)

TOOLCHAIN ?= $(HOME)/tools/arm-gnu-expanded/Payload/bin/
ARMCC = $(TOOLCHAIN)arm-none-eabi-gcc

test:
	@mkdir -p build
	$(HOSTCC) $(HOSTFLAGS) tests/test_pool_host.c src/pool.c src/pool_selftest.c -o build/test_pool_host
	build/test_pool_host

# White-box tests; run with and without spill, in release mode, and on tables whose pools are
# NOT a multiple of the owner granule (padding between pools), one pool, big blocks, other granules,
# and tables that pool_init() must reject.
test-white:
	@mkdir -p build
	$(HOSTCC) $(HOSTFLAGS) tests/test_pool_whitebox.c -o build/test_pool_whitebox
	build/test_pool_whitebox
	$(HOSTCC) $(HOSTFLAGS) -DPOOL_SPILL=1 tests/test_pool_whitebox.c -o build/test_pool_whitebox_spill
	build/test_pool_whitebox_spill
	$(HOSTCC) $(HOSTFLAGS) -DPOOL_DEBUG=0 tests/test_pool_whitebox.c -o build/test_pool_whitebox_release
	build/test_pool_whitebox_release
	@for cfg in "-D'POOL_TABLE(X)=X(8,3)'" "-D'POOL_TABLE(X)=X(8,3) X(16,5) X(32,7)'" \
	            "-DPOOL_SPILL=1 -D'POOL_TABLE(X)=X(8,3) X(16,5) X(32,7)'" "-DPOOL_DEBUG=0 -D'POOL_TABLE(X)=X(8,9) X(64,3) X(1024,2)'" \
	            "-DPOOL_OWNER_GRANULE=8 -D'POOL_TABLE(X)=X(8,3) X(16,5)'" "-DPOOL_OWNER_GRANULE=256 -D'POOL_TABLE(X)=X(8,3) X(16,5) X(512,2)'" \
	            "-DPOOL_EXPECT_BAD_TABLE -D'POOL_TABLE(X)=X(16,2) X(16,3)'" "-DPOOL_EXPECT_BAD_TABLE -D'POOL_TABLE(X)=X(32,2) X(16,3)'" ; do \
	  printf "%-78s" "[$$cfg]"; \
	  eval $(HOSTCC) $(HOSTFLAGS) $$cfg tests/test_pool_whitebox.c -o build/test_pool_whitebox_cfg || exit 1; \
	  build/test_pool_whitebox_cfg | tail -1; \
	done

# The same unit tests under many pool configurations: tiny pools, one pool, big blocks, both debug modes, spill.
test-configs:
	@mkdir -p build
	@for cfg in "" "-DPOOL_DEBUG=0" "-DPOOL_SPILL=1" "-DPOOL_DEBUG=0 -DPOOL_SPILL=1" "-DPOOL_TRACK_PEAK=0" \
	            "-D'POOL_TABLE(X)=X(8,1)'" "-D'POOL_TABLE(X)=X(8,2) X(16,1)'" \
	            "-D'POOL_TABLE(X)=X(8,3)'" "-D'POOL_TABLE(X)=X(8,8) X(16,3) X(32,12)'" \
	            "-D'POOL_TABLE(X)=X(32,100) X(1024,7)'" "-DPOOL_DEBUG=0 -D'POOL_TABLE(X)=X(8,2) X(64,3)'" \
	            "-DPOOL_SPILL=1 -D'POOL_TABLE(X)=X(8,2) X(16,2) X(64,3)'" \
	            "-DPOOL_USER_CONFIG='\"../configs/generic_small.h\"'" ; do \
	  printf "%-62s" "[$$cfg]"; \
	  eval $(HOSTCC) $(HOSTFLAGS) $$cfg tests/test_pool_host.c src/pool.c src/pool_selftest.c -o build/test_pool_cfg || exit 1; \
	  build/test_pool_cfg | tail -1; \
	done

# Exercise the benchmark harness with a fake cycle counter (logic check only).
test-bench:
	@mkdir -p build
	$(HOSTCC) $(HOSTFLAGS) -Itests tests/test_bench_host.c src/bench_common.c src/pool.c -o build/test_bench_host
	build/test_bench_host

test-all: test test-configs test-white test-bench

# Compile (not run!) for several Arm cores, with warnings as errors, in the optimisation levels
# people use, with the options that change the code. Catches things like a Thumb-1 core missing an
# instruction. The library has been RUN only on a Cortex-M4 (real board and QEMU).
CORES = cortex-m0plus cortex-m3 cortex-m4 cortex-m7
check-ports:
	@mkdir -p build/ports
	@set -e; for core in $(CORES); do for opt in -O0 -O2 -Os; do for extra in "" "-DPOOL_DEBUG=0" "-DPOOL_SPILL=1" "-DPOOL_IRQ_SAFE=0"; do \
	  for f in src/pool.c src/pool_selftest.c; do \
	    $(ARMCC) -mcpu=$$core -mthumb -std=gnu11 $$opt -Wall -Wextra -Werror -ffreestanding $(INC) $$extra -c $$f -o build/ports/x.o; \
	  done; \
	done; done; echo "ok   $$core  (-O0 -O2 -Os x debug/release/spill/no-irq-mask)"; done
	@set -e; for core in cortex-m3 cortex-m4 cortex-m7; do \
	  $(ARMCC) -mcpu=$$core -mthumb -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding $(INC) -Iports/cortexm -Itests/arm \
	    -c src/bench_common.c -o build/ports/b.o; echo "ok   $$core  bench harness with the Cortex-M timing windows"; done

# Run the library's self-test on emulated cores (QEMU): the same code, but real 32-bit Thumb
# instructions on Cortex-M0 (Thumb-1, no CLZ instruction), M3, M4 and M7. Needs qemu-system-arm.
# format: machine:cpu:RAM bytes
QEMU ?= qemu-system-arm
QEMU_TARGETS = microbit:cortex-m0:0x4000 mps2-an385:cortex-m3:0x10000 mps2-an386:cortex-m4:0x10000 mps2-an500:cortex-m7:0x10000
QEMU_TIMEOUT ?= 40
test-qemu:
	@mkdir -p build/qemu
	@set -e; for t in $(QEMU_TARGETS); do \
	  machine=$${t%%:*}; rest=$${t#*:}; cpu=$${rest%%:*}; ram=$${rest#*:}; \
	  $(ARMCC) -mcpu=$$cpu -mthumb -std=gnu11 -O2 -Wall -Wextra -ffreestanding -nostdlib -nostartfiles \
	    $(INC) -DPOOL_USER_CONFIG='"../configs/generic_small.h"' \
	    tests/qemu/main.c src/pool.c src/pool_selftest.c -T tests/qemu/link.ld -Wl,--defsym=_ram_len=$$ram -lgcc \
	    -o build/qemu/$$machine.elf; \
	  printf "%-12s %-10s " $$machine $$cpu; \
	  out=$$(perl -e 'alarm shift; exec @ARGV' $(QEMU_TIMEOUT) $(QEMU) -machine $$machine -display none -monitor none \
	    -semihosting-config enable=on,target=native -serial null -kernel build/qemu/$$machine.elf 2>&1) || true; \
	  echo "$$out" | grep -q "QEMU TEST PASSED" && echo "PASSED ($$(echo "$$out" | grep -c '  ok '))" || { echo FAILED; echo "$$out" | tail -n 15; exit 1; }; \
	done

mutate:
	python3 tools/mutate_pool.py

clean:
	rm -rf build

.PHONY: test-qemu test test-white test-configs test-bench test-all check-ports mutate clean
