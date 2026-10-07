CC      ?= cc
CFLAGS  ?= -Wall -Wextra -g -O1
SAN     = -fsanitize=address,undefined -fno-omit-frame-pointer
TSAN    = -fsanitize=thread -fno-omit-frame-pointer
BUILD   = build

PERSIST = mmap/afalloc_persistent.c
PHDR    = mmap/afalloc_persistent.h

.PHONY: all test tsan valgrind bench bench-compare bench-sizes clean
all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_persistent: mmap/test_persistent.c $(PERSIST) $(PHDR) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) mmap/test_persistent.c $(PERSIST) -o $@

$(BUILD)/stress: mmap/stress.c $(PERSIST) $(PHDR) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) mmap/stress.c $(PERSIST) -o $@

$(BUILD)/test_mmap: mmap/test.c mmap/mmap_allocator.c mmap/mmap_allocator.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) mmap/test.c mmap/mmap_allocator.c -o $@

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

test: $(BUILD)/test_persistent $(BUILD)/stress $(BUILD)/test_mmap \
      $(BUILD)/stress_gp $(BUILD)/test_arena_rv1103 $(BUILD)/test_arena_malloc \
      $(BUILD)/stress_arena_malloc
	$(BUILD)/test_persistent
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

clean:
	rm -rf $(BUILD)

# 1 MiB .. 2 GiB chunk sizes; also: make test CFLAGS="-g -O1 -Wall -DAFA_SIZE=4194304ULL" BUILD=build_4m
bench-sizes:
	python3 bench/run_sizes.py

# libc malloc vs every afalloc variant (-O2, no sanitizers); see bench/RESULTS.md
bench-compare:
	python3 bench/run_bench.py
