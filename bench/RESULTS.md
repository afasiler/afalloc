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


### Allocate, ns per allocation

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| 16 B x 100 | 12.6 | 4.1 | 2.2 | 3.9 |
| 16 B x 1,000 | 12.6 | 4.2 | 2.4 | 3.8 |
| 16 B x 10,000 | 13.1 | 4.8 | 2.4 | 4.1 |
| 16 B x 30,000 | 13.1 | 4.9 | 2.4 | 4.1 |
| 256 B x 2,000 | 13.5 | 4.6 | 2.5 | 4.0 |
| 4 KiB x 150 | 23.9 | 4.1 | 2.4 | 4.0 |
| mixed sizes (~900 KiB) | 16.8 | 4.2 | 2.4 | 3.9 |

### Release the whole batch, ns per allocation
malloc frees every block; the others do one bulk reset.

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| 16 B x 100 | 10.3 | 0.1 | 0.1 | 0.1 |
| 16 B x 1,000 | 9.8 | 0.0 | 0.0 | 0.0 |
| 16 B x 10,000 | 9.8 | 0.0 | 0.0 | 0.0 |
| 16 B x 30,000 | 9.7 | 0.0 | 0.0 | 0.0 |
| 256 B x 2,000 | 13.1 | 0.0 | 0.0 | 0.0 |
| 4 KiB x 150 | 19.3 | 0.1 | 0.1 | 0.1 |
| mixed sizes (~900 KiB) | 15.2 | 0.0 | 0.0 | 0.0 |

### Total (alloc + bulk release), ns per allocation
| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) | malloc / persist |
|---|---:|---:|---:|---:|---:|
| 16 B x 100 | 22.8 | 4.2 | 2.4 | 4.1 | 5.5x |
| 16 B x 1,000 | 22.4 | 4.2 | 2.4 | 3.9 | 5.3x |
| 16 B x 10,000 | 22.9 | 4.8 | 2.4 | 4.1 | 4.8x |
| 16 B x 30,000 | 22.8 | 4.9 | 2.4 | 4.1 | 4.7x |
| 256 B x 2,000 | 26.6 | 4.6 | 2.5 | 4.0 | 5.8x |
| 4 KiB x 150 | 43.2 | 4.2 | 2.5 | 4.0 | 10.3x |
| mixed sizes (~900 KiB) | 32.0 | 4.2 | 2.4 | 3.9 | 7.6x |

### First-touch / use (write first+last byte), ns per allocation

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| 16 B x 100 | 0.6 | 0.5 | 0.6 | 0.6 |
| 16 B x 1,000 | 0.6 | 0.6 | 0.6 | 0.6 |
| 16 B x 10,000 | 0.7 | 0.9 | 1.0 | 0.9 |
| 16 B x 30,000 | 0.7 | 0.9 | 1.0 | 0.9 |
| 256 B x 2,000 | 3.1 | 1.6 | 1.4 | 1.5 |
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

### Per-allocation latency, mixed sizes (ns)
Single-op timing; the clock ticks coarsely (see timer floor), so trust the tail, not p50.

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| p50 | 0.0 | 0.0 | 0.0 | 0.0 |
| p99 | 42.0 | 42.0 | 42.0 | 42.0 |
| p99.9 | 125.0 | 83.0 | 42.0 | 42.0 |
| max | 9583.0 | 209.0 | 84.0 | 84.0 |
| timer floor | 0.0 | 0.0 | 0.0 | 0.0 |

### Interleaved alloc/free churn (256 live slots, 16..512 B), ns per op
Bump allocators have no free, so they cannot run this workload.

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|
| ns/op | 26.3 | n/a | n/a | 408.8 |
| failed allocs | 0.0 | n/a | n/a | 0.0 |
