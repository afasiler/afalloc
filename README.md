# afalloc

A dynamic memory allocator written from scratch in C, built to understand what
actually happens underneath `malloc` and `free`.

The repository contains three generations of allocator, written in order. The
newest one, `mmap/afalloc_persistent.c`, is the canonical one.

---

## 1. `mmap/afalloc_persistent.c` — chunked pool allocator (current)

Built for deterministic allocation on small embedded Linux targets (the
Luckfox Pico Mini / RV1103 was the motivating board). Allocation is a bump of
a pointer or a pop from a size-class free list, both O(1). Memory can be
released block by block (`afree`) or in bulk (`afa_reset`).

**Design**

- **Two pools:** *scratch* (`afalloc`) and *persistent* (`afalloc_persistent`).
  Each pool chains up to 10 chunks of 1 MiB, mapped lazily with `mmap`
  (`MAP_PRIVATE | MAP_ANONYMOUS`). `afa_reset()` rewinds the scratch pool only.
- **Segregated size classes:** every multiple of 8 up to 256 bytes, then four
  classes per power of two up to one chunk (at most 25% internal waste). A
  freed block goes onto its class's free list and is handed back, in O(1), to
  the next request of that class. There is no coalescing: blocks keep their
  class for life.
- **Returning memory to the OS:** `afa_trim()` resets scratch and `munmap`s
  every scratch chunk; `afa_destroy()` unmaps both pools.
- **Thread safety:** every public function takes a spinlock (C11
  `atomic_flag`, `sched_yield()` while waiting), so the allocator can be shared
  between threads. Build with `-DAFA_NO_LOCK` to compile it out on a
  single-threaded target.
- **Safe failure:** all sizes are `size_t`. A request that can never fit in a
  chunk (including `SIZE_MAX` and sizes that overflow when rounded) returns
  `NULL` without mapping memory or changing state. `mmap` failure returns
  `NULL`. `afree(NULL)` and a second `afree` of the same block are ignored.
- **Alignment:** the block header is two `size_t` fields (a multiple of 8 bytes
  on 32- and 64-bit targets), so returned pointers are 8-byte aligned.

**API** (`mmap/afalloc_persistent.h`)

```c
void *afalloc(size_t size);            // scratch allocation
void *afalloc_persistent(size_t size); // persistent allocation
void  afree(void *ptr);                // release a block from either pool
void  afa_reset(void);                 // rewind scratch, drop scratch free lists
void  afa_trim(void);                  // reset + munmap scratch chunks
void  afa_destroy(void);               // unmap everything
```

Pointers are invalid after the pool they came from is reset, trimmed or
destroyed; do not `afree` them afterwards.

**Build, test, benchmark**

```bash
make test       # unit tests + randomized stress under ASan/UBSan
make tsan       # multi-threaded test under ThreadSanitizer
make valgrind   # stress driver under Valgrind
make bench      # compare against glibc malloc/free (optimised, no sanitizers)
```

CI (`.github/workflows/ci.yml`) runs `make test` and `make tsan` with gcc and
clang, and `make valgrind`.

- `mmap/stress.c`: 200,000 random operations (allocate, free, reset, trim,
  both pools, sizes from 1 byte to 200 KiB). Every live block gets a unique
  pattern; blocks must be 8-aligned, non-overlapping and intact.
- `mmap/test_persistent.c`: edge cases, including a regression test for the
  oversized-request bug, free-list reuse, double free, chaining and trim.
- `mmap/test_threads.c`: 8 threads allocating and freeing from both pools.
  With the lock compiled out the test no longer completes.

`make bench-compare` runs a wider comparison of libc `malloc`/`free` against
every allocator in the tree (small, mixed and 64 KiB to 1 MiB blocks, latency
tail, alloc/free churn). Results and caveats are in
[`bench/RESULTS.md`](bench/RESULTS.md).

Valgrind does not instrument `mmap`'d regions, so it adds little beyond ASan
here; ASan's redzones are the real overrun check.

**Benchmark** (`make bench`, one run on a 4-core cloud container, gcc `-O2`;
your numbers will differ, and `clock_gettime` adds tens of ns to the per-call
figures):

| Workload | glibc `malloc`/`free` | `afalloc`/`afree` | `afalloc` + `afa_reset` |
|---|---|---|---|
| Bulk: 20,000 small allocs, release all (ns per alloc) | 81.6 | 36.9 | 19.3 |

| Churn: 2M random alloc/free ops (ns per op) | mean | p50 | p99 | p99.99 | max |
|---|---|---|---|---|---|
| glibc | 47.6 | 38 | 167 | 5,456 | 1,335,475 |
| `afalloc`/`afree` | 43.8 | 38 | 126 | 17,526 | 190,419 |

`afalloc` has the lower mean, p99 and worst case here, but a *higher* p99.99
than glibc. Those slow calls are the first touch of freshly `mmap`'d chunks
(a syscall plus page faults), which happens lazily inside `afalloc`. Pre-faulting
the chunks at startup would remove them; that is not implemented. This is a
benchmark of one synthetic workload on one machine, not evidence about the
RV1103 itself.

Also in the tree: `arena_allocator/rv1103.c`, the same bump idea over a single
static 1 MiB array (no `mmap`, no chaining, no free), tested by
`arena_allocator/test_rv1103.c`.

---

## 2. `mmap/mmap_allocator.c` — general-purpose allocator

The earlier general-purpose design over a single 1 MiB `mmap` region, kept for
comparison. Run its tests with `make test` (`mmap/test.c`).

- **Block header:** `size_t` size, a magic number (`0xAFA2BABA`) and a free flag
  (16 bytes on a 64-bit target; the header is 12 bytes on 32-bit, which breaks
  8-byte alignment there).
- **Magic number:** verified on `f_free()` and during traversal so foreign
  pointers and damaged headers are detected.
- **First-fit with splitting** over an implicit list, with a bump frontier.
  A `lowest_free` hint (a block boundary before which everything is in use)
  keeps allocation at the frontier O(1) instead of walking the whole list.
- **Forward coalescing** in `f_free()`, bounded to the range from `lowest_free`
  up to the freed block rather than the whole region.
- **Safe failure:** zero-size, oversized (including `SIZE_MAX`, which used to
  wrap to a 0-byte block when rounded) and `MAP_FAILED` all return `NULL`.

```c
void *afalloc(size_t size);
void  f_free(void *ptr);
void  f_coalescing(void);   // merge adjacent free blocks (f_free already does it)
void  reset_region(void);
```

It defines the same `afalloc` symbol as the other allocators, so link only one
of them into a program.

---

## 3. `arena_allocator/arena_malloc.c` — boundary-tag arena (first version)

The first experiment: a fixed 1 MiB **static array** with boundary tags. Each
block carries a header *and* a footer holding a back-pointer to its header,
which makes constant-time backward coalescing possible. `arena_free()` takes
the user pointer (like `free`) and coalesces in both directions via
`arena_coalesce()`. The capacity check aligns the size first, so a request near
the end of the region can no longer overrun it. Tested by
`arena_allocator/test_arena_malloc.c`.

```bash
cc -Wall -Wextra -g -DARENA_DEMO arena_allocator/arena_malloc.c -o arena && ./arena
```

(`-DARENA_DEMO` builds the old `main()` demo; without it the file is a library.)

---

## Known limitations

- **Free-list blocks never coalesce** and keep their size class, so a workload
  that frees many small blocks and then asks for large ones will not reuse that
  memory until `afa_reset()`.
- **Scratch tail waste:** when a request doesn't fit the remainder of a chunk,
  the rest of that chunk is skipped (until the next reset, apart from blocks
  later returned through `afree`).
- **The lock is a spinlock** held across `mmap`. Under heavy contention on a
  single core, waiters `sched_yield()`; there is no priority inheritance, so it
  is not suitable for hard real-time threads of differing priority.
- **Pools are capped** at 10 chunks (10 MiB) each.
- **Not verified on 32-bit hardware.** The header layout is designed for
  32-bit ARM, but the code has only been built and tested on 64-bit x86-64 Linux.
- **`mmap_allocator.c` assumes a 64-bit header** (12 bytes on 32-bit, which
  breaks 8-byte alignment), has O(n) first-fit once fragmented (about 400 ns
  per op in the churn benchmark) and forward-only coalescing.
- **Fresh chunks fault on first touch**, which shows up in tail latency (see
  the benchmark).

## Possible future work

- Pre-fault chunks at startup (`MAP_POPULATE` / `mlock`) to remove first-touch
  latency.
- Backward coalescing for the free lists.
- Bring `mmap_allocator.c` up to the same safety level, or retire it.
- Run the test suite on real RV1103 hardware.

## Why

Real-time and mission-critical systems often avoid `malloc` outright: its
worst-case latency is unbounded and fragmentation accumulates over long
uptimes. Arena and pool allocators exist because of that. Building one by hand
was the way to understand the trade-off rather than read about it.

## References

Concepts drawn from *Computer Systems: A Programmer's Perspective* (Bryant &
O'Hallaron), chapter 9, and the CMU malloclab problem statement.
