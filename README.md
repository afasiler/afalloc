# afalloc

A dynamic memory allocator written from scratch in C, built to understand what
actually happens underneath `malloc` and `free`.

The repository contains three generations of allocator, written in order. The
newest one, `mmap/afalloc_persistent.c`, is the canonical one.

---

## 1. `mmap/afalloc_persistent.c` — chunked bump allocator (current)

Built for deterministic allocation on small embedded Linux targets (the
Luckfox Pico Mini / RV1103 was the motivating board). There is no `free`:
memory is handed out by bumping a pointer, and scratch memory is released in
bulk with `afa_reset()`.

**Design**

- **Chunks:** memory comes from the kernel in 1 MiB chunks via `mmap`
  (`MAP_PRIVATE | MAP_ANONYMOUS`), up to 10 chunks, mapped lazily on first use.
- **Persistent vs. scratch:** chunk 0 serves `afalloc_persistent()` and is
  never reset. Chunks 1..9 serve `afalloc()` and are rewound by `afa_reset()`.
  Scratch allocation chains automatically: when the current chunk can't hold a
  request, the allocator moves on to the next one.
- **O(1) allocation:** a bump of the chunk frontier plus an 8-byte-aligned
  header (`size_t size; size_t isfree;`, a multiple of 8 bytes on 32- and
  64-bit targets so returned pointers are always 8-byte aligned).
- **Safe failure:** all sizes are `size_t`. A request that can never fit in a
  chunk (including `SIZE_MAX` and sizes that would overflow when rounded) returns
  `NULL` immediately without mapping memory or advancing any state. `mmap`
  failure returns `NULL`.

**API** (`mmap/afalloc_persistent.h`)

```c
void *afalloc(size_t size);            // scratch allocation
void *afalloc_persistent(size_t size); // persistent allocation (chunk 0)
void  afa_reset(void);                 // rewind scratch chunks
```

**Build, test, stress**

```bash
make test       # unit tests + randomized stress under ASan/UBSan
make valgrind   # stress driver under Valgrind
```

`mmap/stress.c` runs 200,000 random allocations (sizes from 1 byte to 200 KiB),
writes a unique pattern into every live block, and checks that every block is
8-aligned, that no two blocks overlap, and that every pattern is intact before
each reset. `mmap/test_persistent.c` covers the edge cases, including a
regression test for the oversized-request bug.

Valgrind does not instrument `mmap`'d regions, so it adds little beyond ASan
here; ASan's redzones are the real overrun check.

Also in the tree: `arena_allocator/rv1103.c`, the same idea over a single
static 1 MiB array (no `mmap`, no chaining, no persistent region), with its own
test in `arena_allocator/test_rv1103.c`.

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
- **Forward coalescing** in `f_free()`.

```c
void *afalloc(size_t size);
void  f_free(void *ptr);
void  reset_region(void);
```

It defines the same `afalloc` symbol as the other allocators, so link only one
of them into a program.

---

## 3. `arena_allocator/arena_malloc.c` — boundary-tag arena (first version)

The first experiment: a fixed 1 MiB **static array** with boundary tags. Each
block carries a header *and* a footer holding a back-pointer to its header,
which makes constant-time backward coalescing possible. `arena_coalesce()`
merges in both directions using this. Its `main()` is a demo.

```bash
cc -Wall -Wextra -g arena_allocator/arena_malloc.c -o arena && ./arena
```

---

## Known limitations

- **Not thread-safe.** No locking and no per-thread arena.
- **No individual `free`** in the current allocator; only bulk reset.
- **Persistent region is a single chunk** (1 MiB); it does not chain.
- **Scratch tail waste:** when a request doesn't fit the remainder of a chunk,
  the rest of that chunk is skipped until the next reset.
- **Memory is never returned to the OS** (no `munmap`).
- **First-fit in `mmap_allocator.c` is O(n)** and its coalescing is
  forward-only. It is also unchecked against `MAP_FAILED`.
- **`arena_malloc.c`:** the size is aligned *after* the capacity check, so a
  request near the end of the region can overrun it; `arena_free()` takes a
  header pointer instead of the user pointer; `arena_coalesce()` is never called
  from `arena_free()`.
- **`mmap_allocator.c` assumes a 64-bit header** (see above).

## Roadmap

1. Run `make test` in CI
2. Fix the `arena_malloc.c` bounds-check ordering
3. Persistent-region chaining and an optional `munmap` path
4. Segregated free lists if individual `free` is ever needed
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
