# afalloc2: a fast, hardened general-purpose allocator

A from-scratch `malloc`-style allocator for 64-bit macOS and Linux, written in C (one file, `afalloc.c`),
built around two goals that usually pull against each other: **speed** and **resistance to heap
memory-safety bugs**. The old `mmap/afalloc_persistent.c` stays in the repository for comparison.

```c
#include "afalloc.h"
void *p = afalloc(100);       afree(p);
void *z = afcalloc(10, 64);    z = afrealloc(z, 4096);
void *a = afaligned_alloc(64, 1000);
size_t n = afusable_size(p);
```

Choose the security level with `AFALLOC_SECURE=0|1|2` (default 1) or `afset_security()` before the first
allocation. Build: `cc -O2 -pthread your.c afalloc.c`. The names are `afalloc`, `afree`, `afcalloc`, `afrealloc`,
`afaligned_alloc`, `afusable_size`, `afget_stats` and `afcheck` (the old `mmap/afalloc_persistent.c` also
uses `afalloc`/`afree`; do not link both).

**What it is not:** it does not replace the system `malloc` (no interposition is provided), it has been run
only on macOS/Apple Silicon (the code is written for Linux too but I have not run it there), and it does not
use hardware features such as ARM MTE or pointer authentication that Apple's own allocator can use.

## Design

```text
reserved heap (16 GiB of address space, PROT_NONE until a slab is used)
  [ slab 0 ][ slab 1 ][ slab 2 ] ...            256 KiB each, aligned; one size class per slab
metadata (separate mapping, guard pages around it)
  [ meta 0 ][ meta 1 ][ meta 2 ] ...            free-list head, counters, owner, keys: never next to the blocks
per thread: a heap with one "current" slab per size class (40 classes, 16 B to 32 KiB)
above 32 KiB: a direct mapping with a guard page on each side, plus a cache of recently freed mappings
```

- **Fast path.** `malloc` is: thread-local heap, size class from a few instructions, pop the slab's free
  list. No lock, no atomic operation. Fresh blocks are threaded onto the list in small batches, lowest
  address first. A freed block goes back on the list of its slab (found by masking the address).
- **Other threads.** A block freed by a thread that does not own its slab is pushed on that slab's lock-free
  "remote" list; the owner collects it when its own list runs dry. A full slab that receives a remote free
  is announced to its owner through a per-heap stack, so its memory is not forgotten.
- **Thread exit.** A thread's slabs are handed to a global list. The free that releases the last live block of
  an abandoned slab returns the whole slab to the pool; a thread that needs that size class before then adopts
  it. All of this happens under the global lock, so a slab cannot be adopted and reused while it is counted.
- **Memory.** Empty slabs are kept (one per class per thread) to avoid a slab being mapped and released at
  its boundary; beyond 64 MiB of empty slabs they are returned with `MADV_FREE`. Freed large mappings
  (up to 8 MiB, 64 MiB in total) are cached for reuse.

## Security levels

| Feature | 0 fast | 1 hardened (default) | 2 paranoid |
|---|:-:|:-:|:-:|
| Metadata outside the heap, in a guarded mapping | yes | yes | yes |
| Free-list links encrypted with a per-slab random key | no | yes | yes |
| Free-list pointer checked before it is followed (range, alignment, same slab) | no | yes | yes |
| `free` validates: inside the heap, slab in use, start of a block | no | yes | yes |
| Immediate double free (`free(p); free(p)`) | no | yes | yes |
| Any double free or free of a never-allocated block (exact allocation bitmap) | no | no | yes |
| Guard page after every slab and around every large block | no | yes | yes |
| Blocks up to 1 KiB zeroed on free | no | no | yes |
| Delayed reuse (32 freed blocks are held and checked for writes after free) | no | no | yes |
| Fresh blocks of a slab handed out in a randomised order | no | no | yes |

Detected corruption prints one line to stderr and aborts. `tests/test_security.c` commits each of these bugs
in a child process and records, per level, whether the allocator caught it (output below, level 0 / 1 / 2).

| Bug | 0 | 1 | 2 |
|---|:-:|:-:|:-:|
| `free(p); free(p)` | missed | caught | caught |
| `free(p); free(q); free(p)` | missed | missed | caught |
| `free(p + 8)` (interior pointer) | missed | caught | caught |
| free of a foreign pointer (global, stack) | missed | caught | caught |
| free of a wild pointer inside the heap range | crash | caught | caught |
| write to a freed block, forging the free-list link | crash | caught | absorbed by the delay |
| last block of a slab overflows by one byte | missed | caught (guard page) | caught (guard page) |
| large block overflows or underflows by one byte | crash | caught (guard page) | caught (guard page) |
| large block double free | missed | caught | caught |
| write into a freed (quarantined) block | missed | missed | caught on release |
| freed data is still readable | readable | readable | zeroed |
| `realloc` of a foreign pointer | missed | caught | caught |

What this does **not** catch, stated plainly: an overflow from one small block into the next one inside a
slab (there are no red zones), a use-after-free read at levels 0 and 1, and anything that never goes through
`free`/`realloc` with a bad pointer. Level 1 misses a double free that is not immediate; level 2 catches it.

## Measurements

Apple M4, macOS, one machine that was also doing other work: read the ratios, not the last digit. "System
malloc" is Apple's own allocator on this macOS (xzone malloc), which does extra security work (zeroing,
type-isolated buckets) that levels 0 and 1 here do not, so the speed comparison is not like for like.
`afalloc (old)` is `mmap/afalloc_persistent.c`. Reproduce with `make bench`.

| Benchmark (ns, lower is better) | system malloc | afalloc (old) | afalloc L0 | afalloc L1 | afalloc L2 |
|---|---:|---:|---:|---:|---:|
| alloc+free pair, 16 B | 14.1 | 5.5 | **4.2** | 6.0 | 7.7 |
| alloc+free pair, 256 B | 14.2 | 5.5 | **4.2** | 6.0 | 16.1 |
| alloc+free pair, 1 KiB | 17.8 | 5.5 | **4.2** | 6.0 | 59.6 |
| alloc+free pair, 8 KiB | 17.2 | 5.5 | **4.3** | 6.1 | 8.8 |
| alloc+free pair, 64 KiB | 67.0 | n/a | **18.0** | 17.8 | 17.8 |
| mixed churn, per operation | 18.0 | 6.3 | **5.7** | 7.7 | 20.7 |
| 8 threads, million pairs per second (higher is better) | 167 (167-349 over 4 runs) | 15 | **1019** | 843 | 526 |
| block freed by another thread, per block | 103 | 90 | **68** | 62 | 87 |
| memory after freeing 90% of 400,000 objects and refilling, MB | 179 | n/a | 95 | 95 | 94 |
| p99.99 latency of one allocation (clock tick is 42 ns) | 125 | 125 | 125 | 166 | 166 |

- Levels 0 and 1 are 2.3 to 4.2 times faster than the system allocator on small blocks (3.3 to 4.2 at level
  0) and several times faster on 8 threads. The system allocator's multi-thread figure varied a lot between my
  runs (167 to 349), so treat that ratio as "about 3 times", not a precise number. The old allocator is fast
  on one thread but collapses with several (one spin lock) and cannot grow past 10 MiB.
- Level 2 costs: about 1.3 times level 1 for 16 B, and 60 ns for 1 KiB blocks because the block is zeroed on
  free and checked again when it leaves the quarantine. If your workload is dominated by blocks of 256 B to
  1 KiB, level 2 is slower than the system allocator; levels 0 and 1 are not.
- The memory row shows the system allocator's footprint growing when blocks are freed at random and the space
  is reused; I did not investigate why. Right after the allocation, afalloc used 94 MB against 97 MB.

## In real programs

The numbers above are the cost of one call. What a whole program gains depends on how much of its time goes
into the allocator. `make realworld` runs SQLite (the system library, with its allocator replaced through the
official `sqlite3_config(SQLITE_CONFIG_MALLOC)` hook) and three small programs written as proxies for a game
loop, a CPU ray tracer and AI work, each with the system allocator and with afalloc at levels 0, 1 and 2.
Runs are interleaved, each cell is the median of 5 runs, and a checksum must be identical in every variant.
Apple M4. The proxies are mine, not real engines or frameworks.

| Program (what it stresses) | allocations | system | afalloc L0 | afalloc L1 | afalloc L2 | L1 vs system |
|---|---:|---:|---:|---:|---:|---:|
| SQLite, in-memory, 250k rows, indexes, group/sort, joins, updates (total ms) | 1.9 million | 769 | 734 | 742 | 788 | **-3.4%** |
| game loop proxy, average frame (ms) | 21.9 million | 2.08 | 1.69 | 1.75 | 1.95 | **-16%** |
| game loop proxy, 99th-percentile frame (ms) | | 2.81 | 2.27 | 2.36 | 2.63 | **-16%** |
| ray tracer: load text asset (ms) | 0.5 million | 174 | 171 | 172 | 172 | -1.5% |
| ray tracer: build BVH (ms) | | 50.4 | 53.3 | 53.6 | 53.9 | +6% (slower) |
| ray tracer: render, no allocation (ms) | | 1038 | 994 | 996 | 987 | -4% |
| AI: MLP inference, per run (ms) | 3.2 million (all in the next row) | 9.17 | 9.16 | 9.17 | 9.18 | 0.0% |
| AI: tokenizer / data pipeline (ms) | 3.2 million | 229 | 190 | 196 | 230 | **-14%** |

What this says:
- A program gains roughly **(number of allocations) x (about 9 ns)**. The game proxy makes 22 million
  allocations in about a second and saves about 175 ms (measured: 190 ms); SQLite makes 1.9 million and saves
  about 17 ms (measured 27 ms); the tokenizer makes 3.2 million and saves about 28 ms (measured 33 ms).
- **Compute-bound work does not change.** The neural-network inference (matrix math, 9 ms) is identical to the
  second decimal; ray tracing without allocation is within a few percent.
- The game proxy is **far more allocation-heavy than a real game**: about 48,000 allocations per frame
  (spatial hash, A* nodes, event objects, particle list nodes). Real engines use pools and arenas and allocate
  far less, so expect a small fraction of that 16%. The p99 frame improving by the same amount is the more
  interesting part: it means no extra spikes.
- It is **not faster everywhere**: building the BVH was 6% slower, and the ray tracer's allocation-free render
  loop was 4% faster, which I attribute to where the tree nodes end up in memory, not to the call speed (I did
  not isolate it). Level 2 gives little or nothing in these programs: its extra work eats the savings.

## Compared with Apple's xzone malloc

Apple's allocator (derived from mimalloc, source in `apple-oss-distributions/libmalloc`) is built for
security and memory efficiency across every process on the system. afalloc2 follows the same family of
ideas (size classes, slabs, per-slab free lists, metadata outside the heap, guard pages, zero-on-free) and
adds exact double-free detection. It does **not** have: type-aware bucket isolation (the compiler passes type
information to Apple's allocator), ARM MTE tagging, pointer-authenticated free-list links, adaptive per-CPU
or per-cluster chunks under contention, or the kernel "deferred reclaim" primitive. Those are features of
being part of the operating system. So the claim here is narrower than "better in every way": for the
benchmarks above, with the security levels described, it is faster; Apple's allocator has hardware-backed
protections this one cannot have.

## Tests, and how far to trust them

| Command | What it does |
|---|---|
| `make test` | sizes, alignment, usable size, contents; calloc, realloc, aligned_alloc; 600,000 random operations with a pattern check on every block; large blocks; 4 producer and 4 consumer threads freeing each other's blocks; blocks that outlive their thread; heap recycling over 400 short-lived threads. At levels 0, 1 and 2 |
| `make test-security` | each bug above, in a child process, at each level; a promised detection that is missed fails the test |
| `make test-soak` | memory stays bounded: 6 rounds of 200,000 blocks freed by other threads, live producers whose consumers free their blocks, and six scenarios where memory must move between size classes (a thread frees 64-byte blocks and 1 KiB blocks then need the same slabs; the owner exits; blocks are freed after the owner exited). Also calls `afcheck()`, which verifies the allocator's own bookkeeping |
| `make test-tsan` | the functional and soak tests under ThreadSanitizer (reduced size): no data race reported at any level |
| `make realworld` | SQLite, a game-loop proxy, a ray tracer and AI proxies with the system allocator and afalloc (median of 5 interleaved runs; checksums must agree) |
| `make mutate` | 45 deliberate bugs injected into `afalloc.c` one at a time; the tests must notice each |

### Mutation testing: do the tests catch bugs?

Each of 45 bugs is injected on its own into a copy of `afalloc.c` and all the tests are run (size classes, slab
geometry, counters, free-list encryption and checks, every security check, the remote-free and thread-exit
paths, the large-block cache, calloc/realloc/aligned_alloc). **44 are caught, 1 survives.** The survivor
("an avail-list slab is taken without first collecting its remote frees") changes no behaviour: the same blocks
are collected when that slab becomes current and its own list runs dry, so no test can tell it from the
original.

The first run caught only 27 of 41; the survivors were real test weaknesses and each led to a better test:
the security tests now require the allocator's **own message** (or a guard-page fault) rather than "the process
died" (a crash on a forged pointer is not detection); a **soak test** checks that memory stays bounded and
moves between size classes; a check that **recycled blocks contain no stale data**; `afcheck()` for the
internal lists. The same measurements exposed a real flaw: after threads exited, about 120 MB of slabs stayed
"in use" even though every block had been freed; abandoned slabs are now returned to the pool by the free that
releases their last block (481 slabs in use after the workload became 3).

## Known limits

- Tested on macOS/Apple Silicon only. Not run on Linux or x86-64.
- `fork()` is not handled (a child inherits locks and thread heaps as they were).
- 16 GiB of address space is reserved at start (falls back to smaller if refused); at most about 7,000
  thread heaps and 24,000 live large blocks.
- A free that races with a thread's exit can leave one slab (256 KiB) counted as live until a thread of that
  size class adopts it. It is never released too early, only held too long; `make test-soak` tolerates up to
  four such slabs.
- No red zones between small blocks, so an overflow inside a slab is not detected (only at slab ends and for
  large blocks).
- Not a drop-in `malloc` replacement: there is no symbol interposition.
