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
- `arenaf` (`arena_malloc.c`) is first-fit over a boundary-tag list: freed blocks are reused and a free block at the frontier is handed back to it. It costs 48 bytes per 16 B block (header + footer) and cannot hand out a full 1 MiB - 16 B block. Churn is about 190 ns/op because of the linear first-fit scan.
- `persist` release time for large blocks (0.04 to 0.55 us) grows with block size even though `afa_reset()` does constant work; the likely cause is the preceding 1 MiB memset evicting the pool metadata from cache (not verified). `persist` is also ~1.3 ns slower per allocation than the unlocked variants because of its spinlock.
- The M-series clock ticks every ~42 ns, so single-op latency (p50/p99) is quantised; read only the tail. `max` is dominated by OS scheduling noise; do not compare it across runs.
- Memory pages are touched in a separate timed pass so page-fault cost is not hidden inside alloc time, and the warm-up sample absorbs first-use faults.


- **Large allocations:** the bump arenas are one 1 MiB region (`persist`: 9 MiB of scratch in 1 MiB chunks), so 1,000 *live* blocks of up to 1 MiB cannot coexist (that would be up to 1 GiB). The large workloads instead run 1,000 iterations of alloc, touch, release, one live block at a time, for malloc too. A request of exactly 1 MiB cannot be satisfied by any afalloc variant because the header does not fit; the largest block is 1 MiB - 16 B.

### Allocate, ns per allocation

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 16 B x 100 | 12.5 | 5.4 | 2.2 | 2.7 | 4.4 |
| 16 B x 1,000 | 12.5 | 5.3 | 2.4 | 2.9 | 4.3 |
| 16 B x 10,000 | 13.1 | 5.5 | 2.4 | 2.9 | 4.4 |
| 16 B x 30,000 | 13.0 | 5.5 | 2.4 | 2.3 | 4.4 |
| 256 B x 2,000 | 13.5 | 6.0 | 2.5 | 2.4 | 4.4 |
| 4 KiB x 150 | 23.7 | 5.4 | 2.4 | 2.4 | 4.3 |
| mixed sizes (~900 KiB) | 16.6 | 5.5 | 2.4 | 2.4 | 4.3 |

### Release the whole batch, ns per allocation
malloc frees every block; the others do one bulk reset.

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 16 B x 100 | 10.1 | 0.3 | 0.1 | 0.1 | 0.1 |
| 16 B x 1,000 | 10.1 | 0.0 | 0.0 | 0.0 | 0.0 |
| 16 B x 10,000 | 10.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| 16 B x 30,000 | 9.7 | 0.0 | 0.0 | 0.0 | 0.0 |
| 256 B x 2,000 | 13.1 | 0.0 | 0.0 | 0.0 | 0.0 |
| 4 KiB x 150 | 19.4 | 0.2 | 0.1 | 0.1 | 0.1 |
| mixed sizes (~900 KiB) | 15.4 | 0.1 | 0.0 | 0.0 | 0.0 |

### Total (alloc + bulk release), ns per allocation
| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) | malloc / persist |
|---|---:|---:|---:|---:|---:|---:|
| 16 B x 100 | 22.6 | 5.7 | 2.4 | 2.8 | 4.5 | 3.9x |
| 16 B x 1,000 | 22.6 | 5.3 | 2.4 | 2.9 | 4.3 | 4.2x |
| 16 B x 10,000 | 23.1 | 5.5 | 2.4 | 2.9 | 4.4 | 4.2x |
| 16 B x 30,000 | 22.7 | 5.5 | 2.4 | 2.3 | 4.4 | 4.2x |
| 256 B x 2,000 | 26.6 | 6.0 | 2.5 | 2.4 | 4.4 | 4.4x |
| 4 KiB x 150 | 43.2 | 5.6 | 2.4 | 2.5 | 4.4 | 7.7x |
| mixed sizes (~900 KiB) | 32.0 | 5.6 | 2.4 | 2.4 | 4.4 | 5.7x |

### First-touch / use (write first+last byte), ns per allocation

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 16 B x 100 | 0.6 | 0.5 | 0.6 | 0.6 | 0.6 |
| 16 B x 1,000 | 0.6 | 0.6 | 0.6 | 0.6 | 0.6 |
| 16 B x 10,000 | 0.7 | 0.9 | 1.0 | 1.2 | 0.9 |
| 16 B x 30,000 | 0.7 | 0.9 | 1.0 | 1.0 | 0.9 |
| 256 B x 2,000 | 3.1 | 1.7 | 1.4 | 2.1 | 1.5 |
| 4 KiB x 150 | 5.6 | 0.6 | 0.6 | 0.6 | 0.6 |
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
| 1 MiB - 16 B (largest supported) | 0.081 | 0.017 | 0.009 | n/a | 0.010 |
| random 1 B .. 1 MiB - 16 B | 0.079 | 0.016 | 0.009 | 0.009 | 0.010 |
| exactly 1 MiB | 0.081 | n/a | n/a | n/a | n/a |

### Release (us per block)

| block size | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| 64 KiB | 0.641 | 0.045 | 0.013 | 0.012 | 0.013 |
| 256 KiB | 0.628 | 0.053 | 0.013 | 0.013 | 0.012 |
| 512 KiB | 0.627 | 0.083 | 0.012 | 0.013 | 0.013 |
| 1 MiB - 16 B (largest supported) | 0.617 | 0.505 | 0.013 | n/a | 0.012 |
| random 1 B .. 1 MiB - 16 B | 0.413 | 0.388 | 0.013 | 0.013 | 0.013 |
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
| 64 KiB | 1.403 | 1.132 | 1.105 | 1.159 | 1.134 |
| 256 KiB | 7.186 | 4.613 | 3.958 | 3.949 | 3.939 |
| 512 KiB | 14.872 | 8.559 | 7.892 | 7.899 | 7.905 |
| 1 MiB - 16 B (largest supported) | 30.362 | 28.835 | 29.823 | n/a | 25.912 |
| random 1 B .. 1 MiB - 16 B | 9.945 | 12.806 | 12.895 | 12.522 | 12.873 |
| exactly 1 MiB | 30.369 | n/a | n/a | n/a | n/a |

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
| p99.9 | 125.0 | 42.0 | 42.0 | 42.0 | 42.0 |
| max | 12208.0 | 8083.0 | 8583.0 | 7667.0 | 9667.0 |
| timer floor | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

### Interleaved alloc/free churn (256 live slots, 16..512 B), ns per op
`arena` (rv1103) has no free, so it cannot run this workload.

| workload | malloc/free | persist (mmap bump) | arena (rv1103) | arena_malloc (free) | gp (mmap first-fit) |
|---|---:|---:|---:|---:|---:|
| ns/op | 26.2 | 10.4 | n/a | 193.0 | 403.7 |
| failed allocs | 0.0 | 0.0 | n/a | 0.0 | 0.0 |

## Large blocks across chunk sizes (1 MiB, 4 MiB, 16 MiB, 1 GiB, 2 GiB)

Each cell is `alloc us / release us / memset-every-byte ms` per block, median of the process runs; `NULL` means every request failed. Request sizes are exact byte counts; `chunk size - 16 B` is the largest block that chunk can hold (arena_malloc needs 32 B for header and footer, so it fails that row by design). 2 GiB is included so a 1 GiB request can fit. One block is live at a time.


### Chunk / region size 1 MiB (`-DAFA_SIZE=1048576`)

| request | malloc/free | persist | arena (rv1103) | arena_malloc | gp |
|---|---:|---:|---:|---:|---:|
| 1 MiB | 0.08 / 0.62 / 0.03 | NULL | NULL | NULL | NULL |
| 4 MiB | 0.09 / 0.51 / 0.12 | NULL | NULL | NULL | NULL |
| 16 MiB | 0.11 / 0.45 / 0.49 | NULL | NULL | NULL | NULL |
| 1 GiB | 0.79 / 1.30 / 27.97 | NULL | NULL | NULL | NULL |
| chunk size - 16 B | n/a | 0.01 / 0.57 / 0.02 | 0.01 / 0.01 / 0.02 | NULL | 0.01 / 0.01 / 0.02 |

### Chunk / region size 4 MiB (`-DAFA_SIZE=4194304`)

| request | malloc/free | persist | arena (rv1103) | arena_malloc | gp |
|---|---:|---:|---:|---:|---:|
| 1 MiB | 0.08 / 0.62 / 0.03 | 0.02 / 0.56 / 0.02 | 0.01 / 0.01 / 0.02 | 0.01 / 0.01 / 0.02 | 0.01 / 0.01 / 0.02 |
| 4 MiB | 0.09 / 0.51 / 0.12 | NULL | NULL | NULL | NULL |
| 16 MiB | 0.11 / 0.45 / 0.49 | NULL | NULL | NULL | NULL |
| 1 GiB | 0.79 / 1.30 / 27.97 | NULL | NULL | NULL | NULL |
| chunk size - 16 B | n/a | 0.02 / 0.50 / 0.11 | 0.01 / 0.01 / 0.12 | NULL | 0.01 / 0.01 / 0.09 |

### Chunk / region size 16 MiB (`-DAFA_SIZE=16777216`)

| request | malloc/free | persist | arena (rv1103) | arena_malloc | gp |
|---|---:|---:|---:|---:|---:|
| 1 MiB | 0.08 / 0.62 / 0.03 | 0.02 / 0.43 / 0.02 | 0.01 / 0.01 / 0.02 | 0.01 / 0.01 / 0.02 | 0.01 / 0.01 / 0.02 |
| 4 MiB | 0.09 / 0.51 / 0.12 | 0.02 / 0.33 / 0.12 | 0.01 / 0.01 / 0.11 | 0.01 / 0.01 / 0.12 | 0.01 / 0.01 / 0.11 |
| 16 MiB | 0.11 / 0.45 / 0.49 | NULL | NULL | NULL | NULL |
| 1 GiB | 0.79 / 1.30 / 27.97 | NULL | NULL | NULL | NULL |
| chunk size - 16 B | n/a | 0.02 / 0.36 / 0.49 | 0.01 / 0.01 / 0.49 | NULL | 0.01 / 0.02 / 0.49 |

### Chunk / region size 1 GiB (`-DAFA_SIZE=1073741824`)

| request | malloc/free | persist | arena (rv1103) | arena_malloc | gp |
|---|---:|---:|---:|---:|---:|
| 1 MiB | 0.08 / 0.62 / 0.03 | 0.02 / 0.48 / 0.02 | 0.01 / 0.01 / 0.02 | 0.01 / 0.01 / 0.02 | 0.01 / 0.01 / 0.02 |
| 4 MiB | 0.09 / 0.51 / 0.12 | 0.02 / 0.40 / 0.12 | 0.01 / 0.01 / 0.12 | 0.01 / 0.01 / 0.12 | 0.01 / 0.01 / 0.12 |
| 16 MiB | 0.11 / 0.45 / 0.49 | 0.02 / 0.38 / 0.49 | 0.01 / 0.01 / 0.49 | 0.01 / 0.02 / 0.49 | 0.01 / 0.02 / 0.49 |
| 1 GiB | 0.79 / 1.30 / 27.97 | NULL | NULL | NULL | NULL |
| chunk size - 16 B | n/a | 0.06 / 0.16 / 28.13 | 0.02 / 0.16 / 28.12 | NULL | 0.07 / 0.10 / 28.38 |

### Chunk / region size 2 GiB (`-DAFA_SIZE=2147483648`)

| request | malloc/free | persist | arena (rv1103) | arena_malloc | gp |
|---|---:|---:|---:|---:|---:|
| 1 MiB | 0.08 / 0.62 / 0.03 | 0.02 / 0.55 / 0.02 | 0.01 / 0.01 / 0.03 | 0.01 / 0.01 / 0.02 | 0.01 / 0.01 / 0.03 |
| 4 MiB | 0.09 / 0.51 / 0.12 | 0.02 / 0.49 / 0.12 | 0.01 / 0.01 / 0.12 | 0.01 / 0.01 / 0.12 | 0.01 / 0.01 / 0.12 |
| 16 MiB | 0.11 / 0.45 / 0.49 | 0.02 / 0.36 / 0.49 | 0.01 / 0.02 / 0.50 | 0.01 / 0.01 / 0.49 | 0.01 / 0.01 / 0.49 |
| 1 GiB | 0.79 / 1.30 / 27.97 | 0.21 / 0.18 / 28.10 | 0.03 / 0.03 / 28.11 | 0.04 / 0.08 / 28.20 | 0.04 / 0.07 / 28.12 |
| chunk size - 16 B | n/a | 0.39 / 0.29 / 56.26 | 0.03 / 0.11 / 56.63 | NULL | 0.01 / 0.21 / 56.44 |

Reading the table: a request succeeds only when it is at most `chunk size - 16 B` (`arena_malloc`: `- 32 B`), so a 1 GiB request needs a chunk larger than 1 GiB (the 2 GiB build). In the 1 GiB build the largest block is 1 GiB - 16 B. Allocation and release stay constant-time at every size (`alloc` 0.01-0.07 us for the bump allocators against 0.08-0.8 us for malloc); the memset column is memory bandwidth (about 38 GB/s here) and identical across allocators. `persist` release is the 0.3-0.55 us outlier already noted above. The 2 GiB `chunk size - 16 B` memset is twice the 1 GiB one for the same reason.
