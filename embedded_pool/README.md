# embedded_pool: a fixed-size block pool allocator for microcontrollers

A small C library (one `.c` file plus headers, no dependencies, no C library needed) that
replaces `malloc`/`free` on a microcontroller with **pools of equally sized blocks**:

- **constant time:** `alloc` and `free` take the same number of steps for every size and however
  many pools there are (on a Cortex-M4 at 96 MHz: 36 and 40 cycles, always);
- **no per-block header:** a 16-byte request costs 16 bytes; the size is implied by the pool;
- **no fragmentation** inside a pool, and the memory is a static array, so the link fails if it
  does not fit instead of the program failing at run time;
- **interrupt safe** (Cortex-M: masks interrupts for a few instructions), optional;
- **debug mode** that catches double frees, writes after free and corrupted free lists.

Hardware lives in two places only (`include/pool_port.h`, and for the benchmark `bench_port.h`);
everything else is plain C. The firmware project that uses the library on a real board is
[`../stm32`](../stm32).

## Using it on your MCU (about five minutes)

1. **Add the files to your build:** `src/pool.c` (and `src/pool_selftest.c` if you want the
   self-test), with `-Iembedded_pool/include`. C11 and a GCC- or Clang-compatible compiler
   (it uses `__builtin_clzl`, `_Static_assert`, `__attribute__`).
2. **Write a config header** (copy one from [`configs/`](configs)) with your pool table, and point
   the build at it:
   ```c
   #define POOL_TABLE(X) \
       X(16, 64)  /* 64 blocks of 16 bytes */ \
       X(64, 16)  /* 16 blocks of 64 bytes */ \
       X(256, 8)
   ```
   ```text
   -DPOOL_USER_CONFIG='"my_pool_config.h"'
   ```
   Sizes are powers of two, at least 8, strictly ascending. Everything you do not set uses the
   defaults in `include/pool_config.h`.
3. **Interrupts.** On an Arm Cortex-M (M0 to M33) the library masks interrupts itself, detected
   from the compiler. On any other CPU either build with `-DPOOL_IRQ_SAFE=0` (use the pool from one
   context only) or supply `-DPOOL_PORT_HEADER='"my_port.h"'` defining two inline functions,
   `pool_irq_save()` and `pool_irq_restore()` (see `include/pool_port.h`).
4. **Placement (optional).** By default the pool memory is an ordinary static array (`.bss`). To
   have the linker show its size and fail the link when RAM is short, define
   `POOL_SECTION_NAME ".pool_mem"` and give your linker script a `NOLOAD` section of that name
   (see `../stm32/linker.ld`).
5. **Call it:**
   ```c
   #include "pool.h"
   pool_init();                       /* once, before anything else; returns -1 for a bad table */
   void *p = pool_alloc(40);          /* from the 64-byte pool */
   pool_free(p);

   struct msg *m = POOL_ALLOC(sizeof *m);   /* size known at build time: faster, see below */
   ```
6. **Check it on your core:** run `make test-qemu` (or flash `pool_selftest()` to the board, as
   `../stm32` does), then read the real peak usage with `pool_get_stats()` to tune the table.

### Fixed-size allocations: `POOL_ALLOC(n)`

When the size is a compile-time constant (an object, a struct, a fixed buffer), write
`POOL_ALLOC(n)` instead of `pool_alloc(n)`. The compiler works out which pool at build time, so the
size-to-pool step disappears from the run-time path: **27 cycles instead of 36** on the Cortex-M4
board, for every size. Under the hood it is `pool_alloc_class(pool_class_of(n))`. Details:
- it needs optimisation on (`-O1` or higher folds the class to a constant; at `-O0` it still works,
  just not faster) and the same pool table in every file that uses it;
- a size of 0 or bigger than the biggest pool returns `NULL` and is counted as `too_big`, like
  `pool_alloc`; with a constant that is a build-time mistake you will see in `pool_get_misc()`;
- `pool_alloc_class(i)` (pool index, 0 = smallest) is public too, for code that picks the pool
  itself; an out-of-range index returns `NULL`;
- freeing is the same call for both: `pool_free(p)`.

### Sizing the pools

The table is the whole design decision. For every kind of object your program allocates, note its
size and the most that are alive at once; put each kind in the smallest pool that holds it. Run the
real workload with `POOL_TRACK_PEAK=1` (default) and read `pool_get_stats()`: `peak` per pool is the
number you need, `alloc_fail` must be 0. A request larger than the biggest pool returns `NULL`
(counted in `pool_get_misc()`), and `POOL_SPILL=1` lets an empty pool borrow from the next bigger
one. Large buffers (for example neural-network tensors) do not belong in pools: give them one arena
of their own.

## Options (all in `include/pool_config.h`, set with `-D` or in your config header)

| Option | Default | Meaning |
|---|---|---|
| `POOL_TABLE(X)` | 5 placeholder pools | the pools: `X(block_size, block_count)` |
| `POOL_USER_CONFIG` | none | header with your settings |
| `POOL_SECTION_NAME` | none (`.bss`) | linker section for the pool memory |
| `POOL_IRQ_SAFE` | 1 | mask interrupts inside alloc/free (costs about 5 cycles) |
| `POOL_PORT_HEADER` | none | your interrupt-mask functions for a non-Cortex-M CPU |
| `POOL_DEBUG` | 1 | double-free bitmap, poison on free, free-list sanity checks, `pool_check()` deep checks. **Not constant time; for development, build with 0 for speed** |
| `POOL_TRACK_PEAK` | 1 | per-pool peak usage in the statistics (costs about 6 cycles per alloc) |
| `POOL_SPILL` | 0 | an empty pool lends from the next larger one |
| `POOL_OWNER_GRANULE` | 64 | granularity of the pointer-to-pool table (power of two >= 8) |
| `POOL_MAX_TOTAL_BYTES` | 96 KiB | compile-time sanity limit on the pools' total size |

## How it works

```text
pool_alloc(n):  k = ceil(log2 n)  (one CLZ instruction)  ->  pool = route[k]  ->  pop the free list
pool_free(p):   granule = (p - arena) / 64  ->  pool = owner_map[granule]  ->  push on the free list
```

The pools share one arena. `route[]` (32 bytes) maps a size class to a pool; `owner_map[]` (one
byte per 64 bytes of pool memory) maps an address to its pool. A free block stores the link to the
next free block in its own first bytes, so nothing is spent on headers; blocks are created lazily
(a counter marks how much of a pool has ever been handed out), so `pool_init()` is cheap. The uncommon
paths (empty free list, refused free) are out of line, so the common path is a short function with no
stack frame. RAM overhead beyond the pools: `owner_map` (pool memory / 64), `route` 32 B, and 64 B
per pool descriptor.

Limits, stated plainly:
- the release build (`POOL_DEBUG=0`) trusts the caller: a double free or a free of a foreign
  pointer is not always detected (foreign and unaligned pointers are refused; a double free is not);
- blocks are not zeroed, and the first word of a fresh block may hold an old free-list link;
- `pool_init()` while blocks are outstanding makes the pool forget them;
- on Cortex-M the pool must not be called from NMI or HardFault (PRIMASK cannot mask them);
- sizes are 32-bit: a single block is at most 2 GiB.

## Tests and how far to trust them

| Command | What it does | Needs |
|---|---|---|
| `make test-all` | everything that runs on this computer (below) | a C compiler |
| `make test` | 13 unit tests under AddressSanitizer + UBSan | |
| `make test-configs` | the unit tests under 13 table/option combinations (tiny, one pool, big blocks, debug on/off, spill, no peak tracking, a user config) | |
| `make test-white` | white-box tests that corrupt the pool's own state: every request size reaches the smallest fitting pool, every byte of the arena maps to the right owner (also padding between pools), `pool_check()` codes, spill accounting, rejection of a table that is not ascending, on 11 table/granule/debug combinations | |
| `make test-bench` | the benchmark harness with a fake cycle counter (logic only) | |
| `make check-ports` | **compiles** (does not run) for Cortex-M0+, M3, M4, M7 at `-O0/-O2/-Os` with warnings as errors, for debug, release, spill and no-irq-mask | `arm-none-eabi-gcc` |
| `make test-qemu` | **runs** the self-test and a PRIMASK-restoration check on emulated Cortex-M0, M3, M4 and M7 | `qemu-system-arm` |
| `make mutate` | injects 56 deliberate bugs into `pool.c` and `pool.h`, one at a time, and checks the tests notice (53 caught, 3 provably equivalent) | both |

What has actually been run: all of the above on this computer (a 64-bit Mac, which also checks that
nothing assumes 32-bit pointers); the self-test on the four emulated cores; the self-test and the
benchmark on a real Cortex-M4 (STM32F411RE, see `../stm32`). Not run: any real Cortex-M0/M3/M7/M33
silicon, other compilers than GCC 15 and Apple clang, other CPU families. A passing test suite is
evidence, not proof: the mutation run shows how strong it is (result in `../stm32/README.md`).

### The benchmark harness

`src/bench_common.c` times any `malloc`/`free` pair (pairs, a batch, a random mixed workload, a
cold-cache case) with a cycle counter and prints min / p50 / mean / p99 / p99.9 / max in net cycles
per call. It needs a header named `bench_port.h` from your project providing the counter and
printing (contract in `include/bench_common.h`; examples: `tests/bench_port.h` for a computer,
`../stm32/src/bench_port.h` for a Cortex-M4). On Cortex-M3/M4/M7 include
`ports/cortexm/bench_window.h` for exact `ldr / blx / ldr` timing windows, and prove them with
`tools/check_bench_windows.py`. Cortex-M0/M0+ have no cycle counter: use a hardware timer in
`bench_cycles()`.

## Layout

```text
include/   pool.h (API)  pool_config.h (options)  pool_port.h (interrupt mask)  pool_selftest.h  bench_common.h
src/       pool.c  pool_selftest.c  bench_common.c
ports/cortexm/bench_window.h      Cortex-M timing windows for the benchmark
configs/   example configs: generic_small.h (M0/M0+ class), stm32f411re.h
tests/     host unit / white-box / bench-logic tests; qemu/ = the emulated-core test program
tools/     mutate_pool.py (mutation testing), check_bench_windows.py
```
