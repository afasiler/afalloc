# afalloc2: build, test, benchmark.
#   make test            functional + stress tests at security levels 0, 1, 2
#   make test-soak       memory stays bounded under heavy and cross-thread use
#   make test-security   every memory-safety bug in a child process: is it caught?
#   make test-tsan       the functional tests under ThreadSanitizer (reduced size; slow)
#   make bench           benchmarks against the system allocator and the old afalloc (macOS)
#   make mutate          inject bugs into afalloc.c and check the tests notice
CC ?= cc
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra -pthread
BUILD = build

test: $(BUILD)/test_afalloc
	@for l in 0 1 2; do AFALLOC_SECURE=$$l perl -e 'alarm shift; exec @ARGV' 300 $(BUILD)/test_afalloc || exit 1; done

$(BUILD)/test_afalloc: tests/test_afalloc.c afalloc.c afalloc.h | $(BUILD)
	$(CC) $(CFLAGS) tests/test_afalloc.c afalloc.c -o $@

test-soak: $(BUILD)/test_soak
	@for l in 0 1 2; do AFALLOC_SECURE=$$l perl -e 'alarm shift; exec @ARGV' 600 $(BUILD)/test_soak || exit 1; done

$(BUILD)/test_soak: tests/test_soak.c afalloc.c afalloc.h | $(BUILD)
	$(CC) $(CFLAGS) tests/test_soak.c afalloc.c -o $@

test-security: $(BUILD)/test_security
	perl -e 'alarm shift; exec @ARGV' 300 $(BUILD)/test_security

$(BUILD)/test_security: tests/test_security.c afalloc.c afalloc.h | $(BUILD)
	$(CC) $(CFLAGS) tests/test_security.c afalloc.c -o $@

test-tsan: | $(BUILD)
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra -pthread -fsanitize=thread tests/test_afalloc.c afalloc.c -o $(BUILD)/test_afalloc_tsan
	@for l in 0 1 2; do AF_TEST_SCALE=10 AFALLOC_SECURE=$$l nice perl -e 'alarm shift; exec @ARGV' 600 $(BUILD)/test_afalloc_tsan || exit 1; done

bench:
	python3 bench/run_bench.py

mutate:
	python3 tools/mutate_afalloc.py

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) build_bench

.PHONY: test test-soak test-security test-tsan bench mutate clean
