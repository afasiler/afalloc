# afalloc

A dynamic memory allocator written from scratch in C, built to understand what
actually happens underneath `malloc` and `free`.

The repository contains two implementations, written in order. The second one
supersedes the first.

---

## 1. `mmap/` — allocator backed by OS memory (current)

Requests a 1 MiB region straight from the kernel with `mmap`
(`MAP_PRIVATE | MAP_ANONYMOUS`) and manages it without the C standard library
allocator.

**Design**

- **Block header (16 bytes):** block size, a magic number, and a free flag.
- **Magic number (`0xAFA2BABA`):** written into every header and verified on
  `f_free()` and during traversal. A pointer that was not produced by this
  allocator, or a header damaged by a buffer overrun, is detected instead of
  silently corrupting the heap.
- **8-byte alignment:** every request is rounded up with `(size + 7) & ~7` so
  returned pointers are suitably aligned for any scalar type.
- **First-fit with splitting:** the region is traversed as an implicit list.
  A free block large enough to hold the request *and* a new header plus at
  least 8 usable bytes is split in two, so oversized blocks are not wasted.
  If no free block fits, a new one is carved off the frontier (bump pointer).
- **Forward coalescing:** `f_free()` marks the block free and then walks the
  region merging each free block with the free block immediately after it,
  which limits external fragmentation from repeated alloc/free cycles.

**API**

```c
void *afalloc(size_t size);   // allocate
void  f_free(void *ptr);      // release, then coalesce
void  reset_region(void);   // reset the whole region to one free block
```

**Build and run**

```bash
cc -Wall -Wextra -g mmap/fastest_allocator_known_to_mankind.c -o afalloc
```

There is no `main()` in this file — link it against your own test driver, or
add one.

---

## 2. `arena_allocator/` — boundary-tag arena (first version)

An earlier, simpler experiment that manages a fixed 1 MiB **static array**
rather than OS-provided memory.

Its purpose was to implement the **boundary tag** technique: each block carries
both a header *and* a footer, and the footer stores a back-pointer to its own
header. That back-pointer is what makes constant-time backward coalescing
possible — given any block, you can find the block before it by stepping back
`sizeof(footer)` bytes and following the pointer, without scanning from the
start of the heap. `arena_coalesce()` merges in both directions using this.

Kept in the repository because the two versions make different trade-offs and
the comparison is the point.

**Build and run**

```bash
cc -Wall -Wextra -g arena_allocator/arena_malloc.c -o arena && ./arena
```

---

## Known limitations

Written down deliberately — these are the things I know are missing, not
things I think are finished.

- **No test suite.** The correctness of an allocator cannot be judged by eye;
  bugs here are silent. The right next step is a randomised stress driver that
  performs long sequences of allocations and frees, writes a known pattern into
  every block, and verifies the pattern is intact on free — run under
  Valgrind and AddressSanitizer.
- **Not thread-safe.** There is no locking and no per-thread arena. Concurrent
  calls will corrupt the block list.
- **Single fixed region.** The `mmap` version maps 1 MiB once and never grows.
  Requests larger than the region, or made after it fills, return `NULL`.
- **Memory is never returned to the OS.** There is no `munmap` path.
- **`mmap` failure is unchecked.** `MAP_FAILED` should be handled rather than
  dereferenced.
- **Coalescing is forward-only** in the `mmap` version. The boundary-tag
  approach from the arena version has not been ported to it yet.
- **First-fit is O(n).** A segregated free list would give far better
  allocation latency, which matters for any workload with a latency bound.
- **Arena version:** the size is aligned *after* the capacity check rather than
  before, so a request near the end of the region can overrun it; `arena_free()`
  takes a header pointer instead of the user pointer; and `arena_coalesce()` is
  never called from `arena_free()`.

## Roadmap

1. Randomised stress test + Valgrind/ASan in CI
2. Fix the arena bounds-check ordering
3. Segregated free lists by size class
4. Backward coalescing in the `mmap` version
5. Basic thread safety
6. Benchmark against glibc `malloc` on an allocation-heavy workload

## Why

Real-time and mission-critical systems often avoid `malloc` outright: its
worst-case latency is unbounded and fragmentation accumulates over long
uptimes. Arena and pool allocators exist because of that. Building one by hand
was the way to understand the trade-off rather than read about it.

## References

Concepts drawn from *Computer Systems: A Programmer's Perspective* (Bryant &
O'Hallaron), chapter 9, and the CMU malloclab problem statement.
