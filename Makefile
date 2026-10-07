CC      ?= cc
CFLAGS  ?= -Wall -Wextra -g -O1
SAN     = -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD   = build

PERSIST = mmap/afalloc_persistent.c

.PHONY: all test valgrind clean
all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_persistent: mmap/test_persistent.c $(PERSIST) mmap/afalloc_persistent.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) mmap/test_persistent.c $(PERSIST) -o $@

$(BUILD)/stress: mmap/stress.c $(PERSIST) mmap/afalloc_persistent.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) mmap/stress.c $(PERSIST) -o $@

$(BUILD)/test_mmap: mmap/test.c mmap/mmap_allocator.c mmap/mmap_allocator.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) mmap/test.c mmap/mmap_allocator.c -o $@

$(BUILD)/test_arena_rv1103: arena_allocator/test_rv1103.c arena_allocator/rv1103.c | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) arena_allocator/test_rv1103.c arena_allocator/rv1103.c -o $@

# plain (unsanitized) build for valgrind
$(BUILD)/stress_plain: mmap/stress.c $(PERSIST) | $(BUILD)
	$(CC) $(CFLAGS) mmap/stress.c $(PERSIST) -o $@

test: $(BUILD)/test_persistent $(BUILD)/stress $(BUILD)/test_mmap $(BUILD)/test_arena_rv1103
	$(BUILD)/test_persistent
	$(BUILD)/stress
	$(BUILD)/test_mmap
	$(BUILD)/test_arena_rv1103

valgrind: $(BUILD)/stress_plain
	valgrind --error-exitcode=1 --leak-check=full $(BUILD)/stress_plain

clean:
	rm -rf $(BUILD)
