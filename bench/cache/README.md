# Cache experiment: does the allocator's placement policy matter?

`make bench-cache` (macOS, single thread, about two minutes). Source: `cache_bench.c`.

Memory does not "live in the cache": the cache holds copies of RAM. An allocator cannot place data in
the cache; what it decides is **which blocks** a program's objects occupy, and that changes how many
pages and cache lines the program touches and whether its accesses look sequential to the hardware
prefetcher.

## What is measured

64-byte objects. (On Apple Silicon a cache line is 128 bytes and a page is 16 KB, so an object is half a
line; I first assumed 64-byte lines, which was wrong for this chip. The "pages" column below counts
4 KiB units, a proxy for spread, not real 16 KB pages.) The program grows to a peak of W live objects, frees a random 7/8,
then keeps cycling between W/8 and W/4 live objects while scanning the survivors in allocation order.
After the first shrink the heap is full of holes, so where the NEW objects go (random holes or the
lowest free addresses) decides how spread out the live set becomes.

| Allocator | Policy |
|---|---|
| malloc | Apple's system allocator (on this macOS: xzone malloc, see below) |
| pool (lib) | `embedded_pool/src/pool.c` as shipped: LIFO free list, statistics on |
| LIFO | minimal LIFO free list (the library's policy, no extras) |
| LIFO+prefetch | the same, prefetching the next free block |
| lowest-first | 3-level bitmap, always hands out the lowest-address free block |

## Results (Apple M4, ns per operation; two runs agreed within about 10%)

Peak working set 128 KB (live set 16-32 KB, fits L1) up to 64 MB (live set 8-16 MB):

| Peak W | alloc ns: malloc / pool / lowest-first | scan ns per object: malloc / pool / lowest-first | pages touched by survivors: pool vs lowest-first |
|---|---|---|---|
| 128 KB | 13.3 / 7.0 / 7.0 | 0.33 / 0.33 / 0.33 | 33 vs 8 |
| 1 MB | 13.1 / 7.6 / 7.0 | 0.64 / 0.62 / 0.86 | 257 vs 64 |
| 8 MB | 12.8 / 8.6 / 6.6 | 0.79 / 0.85 / 0.92 | 2,049 vs 512 |
| 32 MB | 13.0 / 14.2 / 5.7 | 0.91 / 1.12 / 1.00 | 8,192 vs 2,048 |
| 64 MB | 15.1 / **49.7** / 5.0 | 1.58 / **2.71** / 1.50 | 16,382 vs 4,104 |

## What it shows

- While the heap fits in cache and the TLB, the policy barely matters and the LIFO pool beats `malloc`
  (about 7 vs 13 ns).
- Once the peak heap is large (tens of MB), the LIFO pool scatters its live objects over the whole peak
  span (4x more pages), and every allocation lands on a cold random block: **50 ns, 3x slower than
  `malloc`** and 10x slower than address-ordered allocation. Always handing out the lowest free address
  keeps new objects compact and in ascending order, which the hardware prefetcher follows: 5 ns.
- Scanning the survivors is up to 1.8x faster with the compact layout at 64 MB, but slightly SLOWER at
  1-8 MB (0.86 vs 0.62 ns at 1 MB). I did not investigate why (cache-set conflicts when a compact set is
  just above L1 size are a guess).

## How Apple's own allocator works (read from its source, plus probes on this Mac)

macOS ships libmalloc as open source (`apple-oss-distributions/libmalloc`). The default allocator on this
macOS is **xzone malloc**, described in `doc/xzone_malloc.md` of that repository:

- It is **derived from mimalloc** (Microsoft Research): size classes ("bins"), slab chunks made of 16 KB
  slices, 4 MB segments, and per-chunk free lists (blocks up to 4 KB) or bitmaps (4-32 KB).
- Its stated goals are **security and memory efficiency first**: type-isolated buckets, zero-on-free below
  1 KB, metadata kept outside the heap, PAC-signed free-list links, guard pages, ARM MTE tagging.
- For speed it starts with one current chunk per size class and switches to **per-CPU (or per Apple Silicon
  core cluster) chunks** only when it detects contention, and adds **thread caches** for very small sizes
  when volume is high. It uses a kernel "deferred reclaim" ring buffer to give pages back cheaply.
- I searched its source for cache-line, prefetch and false-sharing handling and found **none**. Apple does
  not tune xzone malloc to the cache directly; the cache benefit comes from slab locality (a size class
  served from compact chunks) and from the per-CPU/cluster chunks.

Probes on this Mac: block sizes follow mimalloc-style bins (16, 32, 48, 64, 112, 128, 224, 256, 320, ...);
consecutive `malloc(64)` calls are 64 bytes apart; after freeing a random subset, the blocks come back in
**neither LIFO nor FIFO order (0 of 200 trials each)** but in ascending address order about half the time
(random order would be 1 in 40,320), which fits a compact, address-ordered chunk policy. That is the policy
the "lowest-first" variant above imitates; I did not read the exact mechanism in the 9,500-line source file.

## What it does not show

- One machine, one object size, one synthetic pattern (burst then random shrink); scan order equals
  allocation order, which is an assumption about the program.
- Single thread: no thread caches, locks or false sharing. Real programs differ.
- The machine had other load (load average about 2.5) and macOS cannot pin the experiment to a
  performance core.
- It does not apply to the STM32F411: it has no data cache and 128 KB of RAM. The effect needs a heap
  much larger than the cache or the TLB reach.
