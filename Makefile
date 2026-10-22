CC      ?= cc
CFLAGS  ?= -Wall -Wextra -g -O1
SAN     = -fsanitize=address,undefined -fno-omit-frame-pointer
TSAN    = -fsanitize=thread -fno-omit-frame-pointer
BUILD   = build

PERSIST = mmap/afalloc_persistent.c
PHDR    = mmap/afalloc_persistent.h

.PHONY: all test tsan valgrind bench bench-compare bench-sizes bench-cache clean
all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_persistent: mmap/test_persistent.c $(PERSIST) $(PHDR) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) mmap/test_persistent.c $(PERSIST) -o $@

$(BUILD)/stress: mmap/stress.c $(PERSIST) $(PHDR) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) mmap/stress.c $(PERSIST) -o $@

$(BUILD)/test_mmap: mmap/test.c mmap/mmap_allocator.c mmap/mmap_allocator.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) mmap/test.c mmap/mmap_allocator.c -o $@

$(BUILD)/test_persistent_internal: mmap/test_persistent_internal.c $(PERSIST) $(PHDR) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) mmap/test_persistent_internal.c -o $@

$(BUILD)/stress_gp: mmap/stress_gp.c mmap/mmap_allocator.c mmap/mmap_allocator.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) mmap/stress_gp.c mmap/mmap_allocator.c -o $@

$(BUILD)/test_arena_rv1103: arena_allocator/test_rv1103.c arena_allocator/rv1103.c | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) arena_allocator/test_rv1103.c arena_allocator/rv1103.c -o $@

$(BUILD)/test_arena_malloc: arena_allocator/test_arena_malloc.c arena_allocator/arena_malloc.c arena_allocator/arena_malloc.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) arena_allocator/test_arena_malloc.c arena_allocator/arena_malloc.c -o $@

$(BUILD)/stress_arena_malloc: arena_allocator/stress_arena_malloc.c arena_allocator/arena_malloc.c arena_allocator/arena_malloc.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) arena_allocator/stress_arena_malloc.c arena_allocator/arena_malloc.c -o $@

$(BUILD)/test_threads_tsan: mmap/test_threads.c $(PERSIST) $(PHDR) | $(BUILD)
	$(CC) $(CFLAGS) $(TSAN) mmap/test_threads.c $(PERSIST) -o $@ -lpthread

# plain (unsanitized) builds for valgrind and benchmarking
$(BUILD)/stress_plain: mmap/stress.c $(PERSIST) $(PHDR) | $(BUILD)
	$(CC) $(CFLAGS) mmap/stress.c $(PERSIST) -o $@

$(BUILD)/bench: mmap/bench.c $(PERSIST) $(PHDR) | $(BUILD)
	$(CC) -O2 -Wall -Wextra mmap/bench.c $(PERSIST) -o $@

test: $(BUILD)/test_persistent $(BUILD)/test_persistent_internal $(BUILD)/stress $(BUILD)/test_mmap \
      $(BUILD)/stress_gp $(BUILD)/test_arena_rv1103 $(BUILD)/test_arena_malloc \
      $(BUILD)/stress_arena_malloc
	$(BUILD)/test_persistent
	$(BUILD)/test_persistent_internal
	$(BUILD)/stress
	$(BUILD)/test_mmap
	$(BUILD)/stress_gp
	$(BUILD)/test_arena_rv1103
	$(BUILD)/test_arena_malloc
	$(BUILD)/stress_arena_malloc

tsan: $(BUILD)/test_threads_tsan
	$(BUILD)/test_threads_tsan

valgrind: $(BUILD)/stress_plain
	valgrind --error-exitcode=1 --leak-check=full $(BUILD)/stress_plain

bench: $(BUILD)/bench
	$(BUILD)/bench

# Does the allocator's placement policy change cache use? (macOS only: it uses sysctl and clock_gettime_nsec_np)
# Single thread, about two minutes, run at low priority. See bench/cache/README.md.
bench-cache: | $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -DPOOL_HOST -DPOOL_DEBUG=0 -DPOOL_USER_CONFIG='"cache_bench_pool.h"' \
	      -Iembedded_pool/include -Ibench/cache bench/cache/cache_bench.c embedded_pool/src/pool.c -o $(BUILD)/cache_bench
	nice -n 10 $(BUILD)/cache_bench

clean:
	rm -rf $(BUILD)

# 1 MiB .. 2 GiB chunk sizes; also: make test CFLAGS="-g -O1 -Wall -DAFA_SIZE=4194304ULL" BUILD=build_4m
bench-sizes:
	python3 bench/run_sizes.py

# libc malloc vs every afalloc variant (-O2, no sanitizers); see bench/RESULTS.md
bench-compare:
	python3 bench/run_bench.py
