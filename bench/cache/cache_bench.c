/* Does the allocator's policy change how well the CPU cache is used?
 *
 * Memory does not "live in the cache": the cache holds copies of RAM. What an allocator decides is
 * WHICH blocks a program's objects occupy, and that decides how many cache lines and pages a
 * program touches. So the experiment looks at fragmentation: the program grows to a peak of W live
 * objects, frees a random 7/8, then keeps cycling between W/8 and W/4 live objects while scanning
 * the survivors. Where the new objects go (random holes or the lowest free addresses) decides how
 * spread out the live set is.
 *
 * Allocators (all hand out 64-byte blocks, called through a function pointer):
 *   malloc        the system allocator (Apple libmalloc)
 *   pool (lib)    embedded_pool/src/pool.c as shipped (LIFO free list, stats on, no irq mask on host)
 *   LIFO          a minimal LIFO free list: the freed block is reused first (like the library)
 *   LIFO+prefetch the same, prefetching the next free block
 *   lowest-first  a 3-level bitmap: always hands out the lowest-address free block (compacts)
 *
 * Reported per working set W (peak live objects):
 *   alloc ns   per allocation, including the first write to the block
 *   free ns    per free, including the write of the link into the block
 *   scan ns    per survivor per pass over the survivors (the cache-sensitive number)
 *   pages      distinct 4 KiB pages holding the survivors; span = highest - lowest survivor address
 * Single thread, so thread caches and locks do not matter here. Best of 3 repeats per cell.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <time.h>
#include "pool.h"

#define BLK 64u
#define CAP (1048640u)                  /* blocks in each pool-style allocator */
#define TARGET_ALLOCS 2000000u          /* allocations in the steady state of one cell */
#define REPEATS 3

static inline uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static uint64_t rng_s = 88172645463325252ull;
static inline uint64_t rnd(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return rng_s;
}

static volatile uint64_t sink;

static inline void touch(void *p)
{
    uint64_t *q = p;
    q[0] += 1;
    q[7] += q[0];                       /* both ends of the 64-byte block */
}

/* ---------- allocators ---------- */
typedef struct {
    const char *name;
    void *(*alloc)(void);
    void (*release)(void *);
    void (*reset)(void);
} allocator_t;

static uint8_t *arena;                  /* 64 MB, shared by the toy allocators */

/* malloc */
static void *m_alloc(void) { return malloc(BLK); }
static void m_free(void *p) { free(p); }
static void m_reset(void) {}

/* the shipped library pool */
static void *lib_alloc(void) { return pool_alloc(BLK); }
static void lib_free(void *p) { pool_free(p); }
static void lib_reset(void) { pool_init(); }

/* toy LIFO list */
static void *l_head;
static size_t l_fresh;
static void l_reset(void) { l_head = 0; l_fresh = 0; }
static void *__attribute__((noinline)) l_alloc(void)
{
    void *n = l_head;
    if (n) { l_head = *(void **)n; return n; }
    if (l_fresh < CAP) return arena + (l_fresh++) * BLK;
    return 0;
}
static void *__attribute__((noinline)) lp_alloc(void)
{
    void *n = l_head;
    if (n) { l_head = *(void **)n; __builtin_prefetch(l_head, 1); return n; }
    if (l_fresh < CAP) return arena + (l_fresh++) * BLK;
    return 0;
}
static void __attribute__((noinline)) l_free(void *p) { *(void **)p = l_head; l_head = p; }

/* toy lowest-address-first bitmap (1 = free), three levels */
#define N0 ((CAP + 63u) / 64u)
#define N1 ((N0 + 63u) / 64u)
#define N2 ((N1 + 63u) / 64u)
static uint64_t b0[N0], b1[N1], b2[N2];
static void b_reset(void)
{
    memset(b0, 0xFF, sizeof b0); memset(b1, 0xFF, sizeof b1); memset(b2, 0xFF, sizeof b2);
    if (CAP & 63u) b0[N0 - 1] = (1ull << (CAP & 63u)) - 1;      /* no bits past the last block */
    if (N0 & 63u) b1[N1 - 1] = (1ull << (N0 & 63u)) - 1;
    if (N1 & 63u) b2[N2 - 1] = (1ull << (N1 & 63u)) - 1;
}
static void *__attribute__((noinline)) b_alloc(void)
{
    unsigned i2 = 0;
    while (i2 < N2 && !b2[i2]) i2++;
    if (i2 == N2) return 0;
    unsigned i1 = i2 * 64u + (unsigned)__builtin_ctzll(b2[i2]);
    unsigned i0 = i1 * 64u + (unsigned)__builtin_ctzll(b1[i1]);
    uint64_t w = b0[i0];
    unsigned idx = i0 * 64u + (unsigned)__builtin_ctzll(w);
    w &= w - 1;
    b0[i0] = w;
    if (!w) {
        b1[i1] &= ~(1ull << (i0 & 63u));
        if (!b1[i1]) b2[i2] &= ~(1ull << (i1 & 63u));
    }
    return arena + (size_t)idx * BLK;
}
static void __attribute__((noinline)) b_free(void *p)
{
    unsigned idx = (unsigned)(((uint8_t *)p - arena) / BLK);
    unsigned i0 = idx >> 6, i1 = i0 >> 6, i2 = i1 >> 6;
    b0[i0] |= 1ull << (idx & 63u);
    b1[i1] |= 1ull << (i0 & 63u);
    b2[i2] |= 1ull << (i1 & 63u);
}

static const allocator_t ALLOCS[] = {
    { "malloc",        m_alloc,   m_free, m_reset },
    { "pool (lib)",    lib_alloc, lib_free, lib_reset },
    { "LIFO",          l_alloc,   l_free, l_reset },
    { "LIFO+prefetch", lp_alloc,  l_free, l_reset },
    { "lowest-first",  b_alloc,   b_free, b_reset },
};
#define NALLOC (sizeof ALLOCS / sizeof ALLOCS[0])

/* ---------- the experiment ---------- */
typedef struct { double alloc_ns, free_ns, scan_ns, pages, span_mb; } res_t;

static void **slot, **live;
static uint32_t *perm;

static int cmp_ptr(const void *a, const void *b)
{
    uintptr_t x = *(uintptr_t *)a, y = *(uintptr_t *)b;
    return x < y ? -1 : x > y;
}

static res_t run_cell(const allocator_t *A, unsigned W)
{
    A->reset();
    rng_s = 88172645463325252ull;
    memset(slot, 0, W * sizeof *slot);
    unsigned keep = W / 8;                                       /* survivors after every shrink */
    unsigned rounds = TARGET_ALLOCS / (W / 8u);
    if (rounds < 8) rounds = 8;
    uint64_t ta = 0, tf = 0, ts = 0, na = 0, nf = 0, nsn = 0;
    res_t r = {0, 0, 0, 0, 0};
    unsigned nlive = 0;

    for (unsigned round = 0; round < rounds; round++) {
        /* round 0 grows to the peak (W live objects) and leaves holes all over a W-block span when it
           shrinks; later rounds only regrow to W/4, so WHERE the allocator puts the new objects
           (random holes, or the lowest addresses) decides how spread out the live set becomes. */
        unsigned target = round == 0 ? W : W / 4u;
        uint64_t t0 = now_ns();
        for (unsigned i = 0; i < W && nlive < target; i++) {
            if (slot[i]) continue;
            void *p = A->alloc();
            touch(p);
            slot[i] = p;
            nlive++;
            na++;
        }
        ta += now_ns() - t0;

        unsigned n = 0;                                          /* live slots, then pick survivors */
        for (unsigned i = 0; i < W; i++)
            if (slot[i]) perm[n++] = i;
        for (unsigned i = n - 1; i > 0; i--) {
            unsigned j = (unsigned)(rnd() % (i + 1));
            uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
        }
        t0 = now_ns();
        for (unsigned k = keep; k < n; k++) {                    /* free all but W/8 */
            A->release(slot[perm[k]]);
            slot[perm[k]] = 0;
            nf++;
        }
        tf += now_ns() - t0;
        nlive = keep;

        n = 0;                                                   /* survivors in slot (allocation) order */
        for (unsigned i = 0; i < W; i++)
            if (slot[i]) live[n++] = slot[i];

        unsigned passes = 131072u / n;
        if (passes < 4) passes = 4;
        if (passes > 512) passes = 512;
        t0 = now_ns();
        for (unsigned pass = 0; pass < passes; pass++)
            for (unsigned k = 0; k < n; k++) touch(live[k]);
        ts += now_ns() - t0;
        nsn += (uint64_t)passes * n;

        if (round == rounds - 1) {
            uintptr_t *v = malloc(n * sizeof *v);
            for (unsigned k = 0; k < n; k++) v[k] = (uintptr_t)live[k];
            qsort(v, n, sizeof *v, cmp_ptr);
            for (unsigned k = 1; k < n; k++)
                if (v[k] == v[k - 1]) { printf("BUG: %s handed out the same block twice\n", A->name); exit(2); }
            unsigned pages = 0;
            for (unsigned k = 0; k < n; k++)
                if (k == 0 || (v[k] >> 12) != (v[k - 1] >> 12)) pages++;
            r.pages = pages;
            r.span_mb = (double)(v[n - 1] - v[0]) / (1024.0 * 1024.0);
            free(v);
        }
    }
    for (unsigned i = 0; i < W; i++)
        if (slot[i]) A->release(slot[i]);
    sink += ((uint64_t *)live[0])[0];
    r.alloc_ns = (double)ta / (double)na;
    r.free_ns = (double)tf / (double)nf;
    r.scan_ns = (double)ts / (double)nsn;
    return r;
}

static void print_machine(void)
{
    char brand[128] = "?";
    size_t len = sizeof brand;
    sysctlbyname("machdep.cpu.brand_string", brand, &len, 0, 0);
    uint64_t l1 = 0, l2 = 0;
    len = sizeof l1; sysctlbyname("hw.perflevel0.l1dcachesize", &l1, &len, 0, 0);
    len = sizeof l2; sysctlbyname("hw.perflevel0.l2cachesize", &l2, &len, 0, 0);
    printf("CPU: %s   performance cores: L1D %llu KB, L2 %llu MB (cores cannot be pinned on macOS)\n",
           brand, (unsigned long long)(l1 >> 10), (unsigned long long)(l2 >> 20));
}

int main(void)
{
    arena = mmap(0, (size_t)CAP * BLK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    slot = malloc(CAP * sizeof *slot);
    live = malloc(CAP * sizeof *live);
    perm = malloc(CAP * sizeof *perm);
    if (arena == MAP_FAILED || !slot || !live || !perm) { puts("out of memory"); return 1; }

    print_machine();
    puts("64-byte objects. Each cell: grow to a peak of W live objects, free a random 7/8, scan the survivors; then repeat\n(regrow to W/4, free to W/8, scan). Best of 3. Times include the first write to a new block.");
    static const unsigned WS[] = { 2048, 16384, 131072, 524288, 1048576 };
    for (unsigned wi = 0; wi < sizeof WS / sizeof WS[0]; wi++) {
        unsigned W = WS[wi];
        printf("\nPeak W = %u objects (%.0f KB); steady state: %u..%u live objects (%.0f..%.0f KB)\n", W, W * 64.0 / 1024.0, W / 8, W / 4, W / 8 * 64.0 / 1024.0, W / 4 * 64.0 / 1024.0);
        printf("%-14s %9s %9s %12s %9s %9s\n", "allocator", "alloc ns", "free ns", "scan ns/obj", "4K pages", "span MB");
        for (unsigned ai = 0; ai < NALLOC; ai++) {
            res_t best = {1e18, 1e18, 1e18, 0, 0};
            for (int rep = 0; rep < REPEATS; rep++) {
                res_t r = run_cell(&ALLOCS[ai], W);
                if (r.alloc_ns < best.alloc_ns) best.alloc_ns = r.alloc_ns;
                if (r.free_ns < best.free_ns) best.free_ns = r.free_ns;
                if (r.scan_ns < best.scan_ns) best.scan_ns = r.scan_ns;
                best.pages = r.pages; best.span_mb = r.span_mb;
            }
            printf("%-14s %9.1f %9.1f %12.2f %9.0f %9.2f\n", ALLOCS[ai].name, best.alloc_ns, best.free_ns, best.scan_ns, best.pages, best.span_mb);
            fflush(stdout);
        }
    }
    return (int)(sink & 0);
}
