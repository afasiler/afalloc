# Benchmark results

libc `malloc`/`free` against the four afalloc allocators. Reproduce with `make bench-compare` (`make bench` is the separate, older `mmap/bench.c` vs glibc comparison).

- **Machine:** Apple M4, macOS (arm64), Apple clang 21, `-O2`, no sanitizers, single thread, on battery power
- **Method:** 5 separate process runs per allocator; each reports the median of 9 timed samples (1 warm-up discarded); the tables show the median of those 5
- **Allocators:** `persist` = `mmap/afalloc_persistent.c` (`afalloc`/`afree`, spinlock-guarded, size-class free lists; bulk release is `afa_reset()`), `arena` = `arena_allocator/rv1103.c` (pure bump), `arenaf` = `arena_allocator/arena_malloc.c` (`arena_free` with boundary-tag coalescing), `gp` = `mmap/mmap_allocator.c` (bulk release uses `reset_region()`, churn uses `f_free()`)
- **Workloads** are batch allocate, write first and last byte, release. Every batch fits inside a 1 MiB arena so all allocators run the same sizes. `malloc` has no bulk release and must `free` every block; that cost is real for a "frame/scratch" lifetime pattern and is the point of a bump allocator.
- **Correctness gate:** each binary first allocates 2,000 mixed-size blocks, fills and verifies patterns and alignment; "Failed allocations" must be 0.

## Caveats (read before quoting numbers)

- This is Apple's `libmalloc` on an M4. The motivating target (RV1103: 32-bit Cortex-A7, glibc/uclibc malloc, far lower clock) will show different absolute numbers; the relative gap should be at least as large, but measure on the board before quoting.
- `arena` (rv1103) has **no `free`**, and the others are single-1 MiB-region or small-pool allocators. They win on throughput partly because they do less, not because they are drop-in malloc replacements. The churn workload is the only one that exercises real `free`.
- **`arenaf` (`arena_malloc.c`) cannot reuse freed memory:** `arena_free()` only marks and coalesces headers, and `afalloc()` always bumps the frontier. In the churn workload it ran out of memory (about 3.9 million failed allocations over the run), so its 6.2 ns/op is the cost of returning `NULL`, **not** a valid result. Its `arena_free` header+footer also costs 48 bytes per 16 B block and cannot hand out a full 1 MiB - 16 B block.
- `persist` release time for large blocks (0.04 to 0.55 us) grows with block size even though `afa_reset()` does constant work; the likely cause is the preceding 1 MiB memset evicting the pool metadata from cache (not verified). `persist` is also ~1.3 ns slower per allocation than the unlocked variants because of its spinlock.
- The M-series clock ticks every ~42 ns, so single-op latency (p50/p99) is quantised; read only the tail. `max` is dominated by OS scheduling noise; do not compare it across runs.
- Memory pages are touched in a separate timed pass so page-fault cost is not hidden inside alloc time, and the warm-up sample absorbs first-use faults.


- **Large allocations:** the bump arenas are one 1 MiB region (`persist`: 9 MiB of scratch in 1 MiB chunks), so 1,000 *live* blocks of up to 1 MiB cannot coexist (that would be up to 1 GiB). The large workloads instead run 1,000 iterations of alloc, touch, release, one live block at a time, for malloc too. A request of exactly 1 MiB cannot be satisfied by any afalloc variant because the header does not fit; the largest block is 1 MiB - 16 B.

### Allocate, ns per allocation

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 16 B x 100 | 12.5 | 5.4 | 2.2 | 2.2 | 3.9 |
| 16 B x 1,000 | 12.6 | 5.3 | 2.4 | 2.4 | 3.8 |
| 16 B x 10,000 | 13.1 | 5.5 | 2.2 | 2.4 | 4.1 |
| 16 B x 30,000 | 13.0 | 5.5 | 2.4 | 2.3 | 4.1 |
| 256 B x 2,000 | 13.4 | 6.0 | 2.5 | 3.1 | 4.0 |
| 4 KiB x 150 | 23.9 | 5.4 | 2.3 | 2.4 | 4.0 |
| mixed sizes (~900 KiB) | 16.7 | 5.5 | 2.4 | 2.4 | 3.9 |

### Release the whole batch, ns per allocation
malloc frees every block; the others do one bulk reset.

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 16 B x 100 | 10.1 | 0.3 | 0.1 | 0.1 | 0.1 |
| 16 B x 1,000 | 9.7 | 0.0 | 0.0 | 0.0 | 0.0 |
| 16 B x 10,000 | 10.2 | 0.0 | 0.0 | 0.0 | 0.0 |
| 16 B x 30,000 | 10.2 | 0.0 | 0.0 | 0.0 | 0.0 |
| 256 B x 2,000 | 13.2 | 0.0 | 0.0 | 0.0 | 0.0 |
| 4 KiB x 150 | 19.3 | 0.2 | 0.1 | 0.1 | 0.1 |
| mixed sizes (~900 KiB) | 15.4 | 0.1 | 0.0 | 0.0 | 0.0 |

### Total (alloc + bulk release), ns per allocation
| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) | malloc / persist |
|---|---:|---:|---:|---:|---:|---:|
| 16 B x 100 | 22.6 | 5.7 | 2.4 | 2.4 | 4.1 | 3.9x |
| 16 B x 1,000 | 22.3 | 5.3 | 2.4 | 2.4 | 3.9 | 4.2x |
| 16 B x 10,000 | 23.3 | 5.5 | 2.2 | 2.4 | 4.1 | 4.2x |
| 16 B x 30,000 | 23.3 | 5.5 | 2.4 | 2.3 | 4.1 | 4.3x |
| 256 B x 2,000 | 26.7 | 6.0 | 2.5 | 3.1 | 4.0 | 4.4x |
| 4 KiB x 150 | 43.2 | 5.6 | 2.4 | 2.5 | 4.1 | 7.8x |
| mixed sizes (~900 KiB) | 32.1 | 5.6 | 2.4 | 2.4 | 3.9 | 5.7x |

### First-touch / use (write first+last byte), ns per allocation

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 16 B x 100 | 0.6 | 0.5 | 0.6 | 0.6 | 0.6 |
| 16 B x 1,000 | 0.6 | 0.6 | 0.6 | 0.6 | 0.6 |
| 16 B x 10,000 | 0.7 | 0.9 | 1.0 | 1.2 | 0.9 |
| 16 B x 30,000 | 0.7 | 0.9 | 1.0 | 1.0 | 0.9 |
| 256 B x 2,000 | 3.1 | 1.7 | 1.4 | 2.2 | 1.5 |
| 4 KiB x 150 | 5.7 | 0.6 | 0.6 | 0.6 | 0.6 |
| mixed sizes (~900 KiB) | 1.2 | 0.6 | 0.6 | 0.6 | 0.6 |

### Failed allocations (must be 0)

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 16 B x 100 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| 16 B x 1,000 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| 16 B x 10,000 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| 16 B x 30,000 | 0.0 | 0.0 | 0.0 | 244650.0 | 0.0 |
| 256 B x 2,000 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| 4 KiB x 150 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| mixed sizes (~900 KiB) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

### Address-space cost of a 16 B allocation (bytes)

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| bytes per 16 B alloc | 16.0 | 32.0 | 32.0 | 48.0 | 32.0 |

## Large allocations: 1,000 iterations of alloc, touch, release per block size
Times are microseconds per block. One block is live at a time because the bump arenas are a single 1 MiB region.

### Allocate (us per block)

| block size | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 64 KiB | 0.079 | 0.017 | 0.009 | 0.009 | 0.010 |
| 256 KiB | 0.081 | 0.017 | 0.009 | 0.009 | 0.010 |
| 512 KiB | 0.081 | 0.017 | 0.009 | 0.009 | 0.010 |
| 1 MiB - 16 B (largest supported) | 0.082 | 0.017 | 0.009 | n/a | 0.010 |
| random 1 B .. 1 MiB - 16 B | 0.080 | 0.017 | 0.009 | 0.009 | 0.010 |
| exactly 1 MiB | 0.082 | n/a | n/a | n/a | n/a |

### Release (us per block)

| block size | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 64 KiB | 0.640 | 0.043 | 0.012 | 0.013 | 0.013 |
| 256 KiB | 0.629 | 0.058 | 0.013 | 0.012 | 0.013 |
| 512 KiB | 0.624 | 0.140 | 0.012 | 0.012 | 0.013 |
| 1 MiB - 16 B (largest supported) | 0.613 | 0.551 | 0.013 | n/a | 0.013 |
| random 1 B .. 1 MiB - 16 B | 0.351 | 0.406 | 0.013 | 0.012 | 0.013 |
| exactly 1 MiB | 0.617 | n/a | n/a | n/a | n/a |

### Touch first and last byte (us per block)

| block size | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 64 KiB | 0.013 | 0.013 | 0.013 | 0.013 | 0.013 |
| 256 KiB | 0.013 | 0.013 | 0.013 | 0.013 | 0.013 |
| 512 KiB | 0.013 | 0.013 | 0.013 | 0.013 | 0.013 |
| 1 MiB - 16 B (largest supported) | 0.013 | 0.013 | 0.013 | n/a | 0.013 |
| random 1 B .. 1 MiB - 16 B | 0.013 | 0.013 | 0.013 | 0.013 | 0.013 |
| exactly 1 MiB | 0.013 | n/a | n/a | n/a | n/a |

### Write every byte with memset (us per block)
Includes page-fault cost on fresh pages; bump arenas reuse already-faulted pages.

| block size | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 64 KiB | 1.415 | 0.991 | 1.112 | 1.006 | 1.088 |
| 256 KiB | 7.187 | 3.944 | 3.974 | 3.961 | 4.481 |
| 512 KiB | 14.897 | 7.900 | 10.250 | 8.137 | 10.030 |
| 1 MiB - 16 B (largest supported) | 30.465 | 29.037 | 30.749 | n/a | 30.797 |
| random 1 B .. 1 MiB - 16 B | 9.689 | 12.662 | 13.001 | 12.300 | 12.899 |
| exactly 1 MiB | 30.384 | n/a | n/a | n/a | n/a |

### Failed allocations per 1,000 requests
| block size | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 64 KiB | 0 | 0 | 0 | 0 | 0 |
| 256 KiB | 0 | 0 | 0 | 0 | 0 |
| 512 KiB | 0 | 0 | 0 | 0 | 0 |
| 1 MiB - 16 B (largest supported) | 0 | 0 | 0 | 1000 | 0 |
| random 1 B .. 1 MiB - 16 B | 0 | 0 | 0 | 0 | 0 |
| exactly 1 MiB | 0 | 1000 | 1000 | 1000 | 1000 |

### Per-allocation latency, mixed sizes (ns)
Single-op timing; the clock ticks coarsely (see timer floor), so trust the tail, not p50.

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| p50 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| p99 | 42.0 | 42.0 | 42.0 | 42.0 | 42.0 |
| p99.9 | 166.0 | 42.0 | 42.0 | 42.0 | 42.0 |
| max | 8458.0 | 11917.0 | 3708.0 | 84.0 | 8125.0 |
| timer floor | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

### Interleaved alloc/free churn (256 live slots, 16..512 B), ns per op
`arena` (rv1103) has no free, so it cannot run this workload. `arenaf` runs out of memory here (see caveats), so ignore its ns/op.

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| ns/op | 26.2 | 10.5 | n/a | 6.2 | 408.0 |
| failed allocs | 0.0 | 0.0 | n/a | 3930028.0 | 0.0 |
