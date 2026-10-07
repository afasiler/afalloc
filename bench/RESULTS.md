# Benchmark results

libc `malloc`/`free` against the three afalloc allocators. Reproduce with `make bench`.

- **Machine:** Apple M4, macOS (arm64), Apple clang 21, `-O2`, no sanitizers, single thread, on battery power
- **Method:** 5 separate process runs per allocator; each reports the median of 9 timed samples (1 warm-up discarded); the tables show the median of those 5
- **Allocators:** `persist` = `mmap/afalloc_persistent.c` (`afalloc`), `arena` = `arena_allocator/rv1103.c`, `gp` = `mmap/mmap_allocator.c` (bulk release uses `reset_region()`, churn uses `f_free()`)
- **Workloads** are batch allocate, write first and last byte, release. Every batch fits inside a 1 MiB arena so all allocators run the same sizes. `malloc` has no bulk release and must `free` every block; that cost is real for a "frame/scratch" lifetime pattern and is the point of a bump allocator.
- **Correctness gate:** each binary first allocates 2,000 mixed-size blocks, fills and verifies patterns and alignment; "Failed allocations" must be 0.

## Caveats (read before quoting numbers)

- This is Apple's `libmalloc` on an M4. The motivating target (RV1103: 32-bit Cortex-A7, glibc/uclibc malloc, far lower clock) will show different absolute numbers; the relative gap should be at least as large, but measure on the board before quoting.
- Bump allocators here have **no `free`**. They win on throughput because they do less, not because they are a drop-in malloc replacement. The churn workload is the only one that exercises real `free`.
- The M-series clock ticks every ~42 ns, so single-op latency (p50/p99) is quantised; read only the tail. `max` is dominated by OS scheduling noise; do not compare it across runs.
- Memory pages are touched in a separate timed pass so page-fault cost is not hidden inside alloc time, and the warm-up sample absorbs first-use faults.


- **Large allocations:** the bump arenas are one 1 MiB region (`persist`: 9 MiB of scratch in 1 MiB chunks), so 1,000 *live* blocks of up to 1 MiB cannot coexist (that would be up to 1 GiB). The large workloads instead run 1,000 iterations of alloc, touch, release, one live block at a time, for malloc too. A request of exactly 1 MiB cannot be satisfied by any afalloc variant because the header does not fit; the largest block is 1 MiB - 16 B.

### Allocate, ns per allocation

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| 16 B x 100 | 12.5 | 4.1 | 2.2 | 4.0 |
| 16 B x 1,000 | 12.5 | 4.2 | 2.4 | 3.9 |
| 16 B x 10,000 | 13.1 | 4.9 | 2.4 | 4.1 |
| 16 B x 30,000 | 13.1 | 5.0 | 2.4 | 4.2 |
| 256 B x 2,000 | 13.5 | 4.7 | 2.5 | 4.0 |
| 4 KiB x 150 | 24.1 | 4.2 | 2.3 | 4.0 |
| mixed sizes (~900 KiB) | 16.9 | 4.2 | 2.4 | 3.9 |

### Release the whole batch, ns per allocation
malloc frees every block; the others do one bulk reset.

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| 16 B x 100 | 10.0 | 0.1 | 0.1 | 0.1 |
| 16 B x 1,000 | 9.7 | 0.0 | 0.0 | 0.0 |
| 16 B x 10,000 | 10.2 | 0.0 | 0.0 | 0.0 |
| 16 B x 30,000 | 9.7 | 0.0 | 0.0 | 0.0 |
| 256 B x 2,000 | 12.8 | 0.0 | 0.0 | 0.0 |
| 4 KiB x 150 | 19.3 | 0.1 | 0.1 | 0.1 |
| mixed sizes (~900 KiB) | 15.4 | 0.0 | 0.0 | 0.0 |

### Total (alloc + bulk release), ns per allocation
| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) | malloc / persist |
|---|---:|---:|---:|---:|---:|
| 16 B x 100 | 22.5 | 4.2 | 2.4 | 4.1 | 5.3x |
| 16 B x 1,000 | 22.2 | 4.2 | 2.4 | 3.9 | 5.3x |
| 16 B x 10,000 | 23.2 | 4.9 | 2.4 | 4.1 | 4.8x |
| 16 B x 30,000 | 22.8 | 5.0 | 2.4 | 4.2 | 4.6x |
| 256 B x 2,000 | 26.3 | 4.7 | 2.5 | 4.0 | 5.6x |
| 4 KiB x 150 | 43.4 | 4.3 | 2.4 | 4.0 | 10.0x |
| mixed sizes (~900 KiB) | 32.3 | 4.3 | 2.4 | 3.9 | 7.6x |

### First-touch / use (write first+last byte), ns per allocation

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| 16 B x 100 | 0.6 | 0.5 | 0.6 | 0.6 |
| 16 B x 1,000 | 0.6 | 0.6 | 0.6 | 0.6 |
| 16 B x 10,000 | 0.7 | 0.9 | 1.0 | 0.9 |
| 16 B x 30,000 | 0.7 | 0.9 | 1.0 | 0.9 |
| 256 B x 2,000 | 3.1 | 1.5 | 1.4 | 1.5 |
| 4 KiB x 150 | 5.7 | 0.6 | 0.6 | 0.6 |
| mixed sizes (~900 KiB) | 1.2 | 0.6 | 0.6 | 0.6 |

### Failed allocations (must be 0)

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| 16 B x 100 | 0.0 | 0.0 | 0.0 | 0.0 |
| 16 B x 1,000 | 0.0 | 0.0 | 0.0 | 0.0 |
| 16 B x 10,000 | 0.0 | 0.0 | 0.0 | 0.0 |
| 16 B x 30,000 | 0.0 | 0.0 | 0.0 | 0.0 |
| 256 B x 2,000 | 0.0 | 0.0 | 0.0 | 0.0 |
| 4 KiB x 150 | 0.0 | 0.0 | 0.0 | 0.0 |
| mixed sizes (~900 KiB) | 0.0 | 0.0 | 0.0 | 0.0 |

### Address-space cost of a 16 B allocation (bytes)

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| bytes per 16 B alloc | 16.0 | 32.0 | 32.0 | 32.0 |

## Large allocations: 1,000 iterations of alloc, touch, release per block size
Times are microseconds per block. One block is live at a time because the bump arenas are a single 1 MiB region.

### Allocate (us per block)

| block size | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| 64 KiB | 0.079 | 0.010 | 0.009 | 0.010 |
| 256 KiB | 0.081 | 0.010 | 0.009 | 0.010 |
| 512 KiB | 0.081 | 0.011 | 0.009 | 0.010 |
| 1 MiB - 16 B (largest supported) | 0.082 | 0.012 | 0.009 | 0.010 |
| random 1 B .. 1 MiB - 16 B | 0.080 | 0.010 | 0.009 | 0.011 |
| exactly 1 MiB | 0.082 | n/a | n/a | n/a |

### Release (us per block)

| block size | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| 64 KiB | 0.640 | 0.013 | 0.013 | 0.013 |
| 256 KiB | 0.621 | 0.013 | 0.013 | 0.013 |
| 512 KiB | 0.613 | 0.013 | 0.012 | 0.013 |
| 1 MiB - 16 B (largest supported) | 0.591 | 0.015 | 0.013 | 0.013 |
| random 1 B .. 1 MiB - 16 B | 0.467 | 0.014 | 0.013 | 0.013 |
| exactly 1 MiB | 0.592 | n/a | n/a | n/a |

### Touch first and last byte (us per block)

| block size | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| 64 KiB | 0.013 | 0.013 | 0.013 | 0.013 |
| 256 KiB | 0.013 | 0.013 | 0.013 | 0.013 |
| 512 KiB | 0.013 | 0.013 | 0.013 | 0.013 |
| 1 MiB - 16 B (largest supported) | 0.013 | 0.013 | 0.013 | 0.013 |
| random 1 B .. 1 MiB - 16 B | 0.013 | 0.014 | 0.013 | 0.013 |
| exactly 1 MiB | 0.013 | n/a | n/a | n/a |

### Write every byte with memset (us per block)
This column is memory-bandwidth bound and varies by up to ~30% between runs (CPU frequency, cache state); the allocators do not meaningfully differ here. Trust the alloc and release tables, not this one.
Includes page-fault cost on fresh pages; bump arenas reuse already-faulted pages.

| block size | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| 64 KiB | 1.426 | 1.044 | 1.080 | 1.099 |
| 256 KiB | 7.247 | 4.098 | 4.541 | 4.656 |
| 512 KiB | 15.003 | 10.539 | 7.897 | 7.987 |
| 1 MiB - 16 B (largest supported) | 30.648 | 28.965 | 20.079 | 20.096 |
| random 1 B .. 1 MiB - 16 B | 12.753 | 12.734 | 12.745 | 14.327 |
| exactly 1 MiB | 30.671 | n/a | n/a | n/a |

### Failed allocations per 1,000 requests
| block size | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| 64 KiB | 0 | 0 | 0 | 0 |
| 256 KiB | 0 | 0 | 0 | 0 |
| 512 KiB | 0 | 0 | 0 | 0 |
| 1 MiB - 16 B (largest supported) | 0 | 0 | 0 | 0 |
| random 1 B .. 1 MiB - 16 B | 0 | 0 | 0 | 0 |
| exactly 1 MiB | 0 | 1000 | 1000 | 1000 |

### Per-allocation latency, mixed sizes (ns)
Single-op timing; the clock ticks coarsely (see timer floor), so trust the tail, not p50.

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| p50 | 0.0 | 0.0 | 0.0 | 0.0 |
| p99 | 42.0 | 42.0 | 42.0 | 42.0 |
| p99.9 | 125.0 | 83.0 | 42.0 | 42.0 |
| max | 12125.0 | 19625.0 | 6541.0 | 9792.0 |
| timer floor | 0.0 | 0.0 | 0.0 | 0.0 |

### Interleaved alloc/free churn (256 live slots, 16..512 B), ns per op
Bump allocators have no free, so they cannot run this workload.

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| ns/op | 26.4 | n/a | n/a | 411.7 |
| failed allocs | 0.0 | n/a | n/a | 0.0 |
