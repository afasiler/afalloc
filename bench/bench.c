/* Benchmark: libc malloc/free vs the afalloc variants.
 *
 * Every allocator exports the same `afalloc` symbol, so one backend is chosen
 * at compile time (-DBE_MALLOC, -DBE_PERSIST, -DBE_ARENA or -DBE_GP) and
 * bench/run_bench.py builds and runs one binary per backend.
 *
 * Output: CSV lines "backend,workload,metric,value" on stdout.
 */
#define _POSIX_C_SOURCE 200809L
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(BE_MALLOC)
#define BE_NAME "malloc"
#define BE_HAS_FREE 1
static inline void *be_alloc(size_t n) { return malloc(n); }
static inline void be_free(void *p) { free(p); }
/* malloc has no bulk release: every block is freed individually */
static void be_release(void **p, size_t n) { for (size_t i = 0; i < n; i++) free(p[i]); }

#elif defined(BE_PERSIST)
#include "afalloc_persistent.h"
#define BE_NAME "persist"
#define BE_HAS_FREE 0
static inline void *be_alloc(size_t n) { return afalloc(n); }
static inline void be_free(void *p) { (void)p; } /* unused: no individual free */
static void be_release(void **p, size_t n) { (void)p; (void)n; afa_reset(); }

#elif defined(BE_ARENA)
#define BE_NAME "arena"
#define BE_HAS_FREE 0
void *afalloc(size_t size);
void afa_reset(void);
static inline void *be_alloc(size_t n) { return afalloc(n); }
static inline void be_free(void *p) { (void)p; } /* unused: no individual free */
static void be_release(void **p, size_t n) { (void)p; (void)n; afa_reset(); }

#elif defined(BE_GP)
#include "mmap_allocator.h"
#define BE_NAME "gp"
#define BE_HAS_FREE 1
static inline void *be_alloc(size_t n) { return afalloc(n); }
static inline void be_free(void *p) { f_free(p); }
static void be_release(void **p, size_t n) { (void)p; (void)n; reset_region(); }

#else
#error "define one of BE_MALLOC BE_PERSIST BE_ARENA BE_GP"
#endif

#define SAMPLES 9            /* timed samples per workload; median is reported */
#define MAX_BATCH 40000
#define HEADER 16            /* afalloc header on a 64-bit target */
#define REGION_BUDGET (900u * 1024u) /* keep a batch inside the 1 MiB arenas */

/* clock_gettime(CLOCK_MONOTONIC) only has microsecond resolution on macOS,
   which makes per-op latency useless; use the raw tick counter there. */
#if defined(__APPLE__)
#include <mach/mach_time.h>
static uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
#else
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static void *ptrs[MAX_BATCH];
static size_t sizes[MAX_BATCH];

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double median(double *v, int n) {
    qsort(v, (size_t)n, sizeof *v, cmp_d);
    return v[n / 2];
}

static void emit(const char *workload, const char *metric, double v) {
    printf("%s,%s,%s,%.3f\n", BE_NAME, workload, metric, v);
}

/* ---- batch workloads: allocate a batch, touch it, release it ------------- */

static void run_batch(const char *name, size_t n, int reps) {
    double a[SAMPLES], t[SAMPLES], r[SAMPLES];
    size_t fails = 0;
    for (int s = -1; s < SAMPLES; s++) {       /* s == -1 is a discarded warm-up */
        uint64_t ta = 0, tt = 0, tr = 0;
        for (int k = 0; k < reps; k++) {
            uint64_t t0 = now_ns();
            for (size_t i = 0; i < n; i++) ptrs[i] = be_alloc(sizes[i]);
            uint64_t t1 = now_ns();
            for (size_t i = 0; i < n; i++) {
                if (!ptrs[i]) { fails++; continue; }
                unsigned char *p = ptrs[i];
                p[0] = 1; p[sizes[i] - 1] = 2;
            }
            uint64_t t2 = now_ns();
            be_release(ptrs, n);
            uint64_t t3 = now_ns();
            ta += t1 - t0; tt += t2 - t1; tr += t3 - t2;
        }
        if (s < 0) continue;
        double ops = (double)n * reps;
        a[s] = ta / ops; t[s] = tt / ops; r[s] = tr / ops;
    }
    emit(name, "alloc_ns", median(a, SAMPLES));
    emit(name, "touch_ns", median(t, SAMPLES));
    emit(name, "release_ns", median(r, SAMPLES));
    emit(name, "failed_allocs", (double)fails);
}

static void fill_fixed(size_t n, size_t sz) {
    for (size_t i = 0; i < n; i++) sizes[i] = sz;
}

/* sizes: 70% 1..128, 25% 1..8192, 5% 1..32768, capped to the arena budget */
static size_t fill_mixed(void) {
    size_t n = 0, used = 0;
    while (n < MAX_BATCH) {
        uint64_t r = rnd() % 100;
        size_t sz = r < 70 ? 1 + rnd() % 128 : r < 95 ? 1 + rnd() % 8192 : 1 + rnd() % 32768;
        size_t cost = ((sz + 7) & ~(size_t)7) + HEADER;
        if (used + cost > REGION_BUDGET) break;
        sizes[n++] = sz;
        used += cost;
    }
    return n;
}

/* ---- address-space cost of a 16-byte allocation ------------------------- */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void run_density(void) {
    enum { N = 1000 };
    static uint64_t delta[N];
    for (size_t i = 0; i < N; i++) {
        ptrs[i] = be_alloc(16);
        assert(ptrs[i]);
    }
    /* median distance between consecutive allocations: malloc scatters blocks
       across size-class regions, so first-to-last span would be meaningless */
    for (size_t i = 1; i < N; i++) {
        uintptr_t a = (uintptr_t)ptrs[i - 1], b = (uintptr_t)ptrs[i];
        delta[i - 1] = a > b ? a - b : b - a;
    }
    qsort(delta, N - 1, sizeof delta[0], cmp_u64);
    emit("density_16B", "bytes_per_alloc", (double)delta[(N - 1) / 2]);
    be_release(ptrs, N);
}

/* ---- per-allocation latency (tail behaviour) ---------------------------- */

static void run_latency(void) {
    size_t n = fill_mixed();
    int reps = 400;
    uint64_t *lat = malloc(sizeof(uint64_t) * n * (size_t)reps);
    assert(lat);
    size_t m = 0;

    /* timer floor: back-to-back reads. On Apple Silicon the clock ticks every
       ~41 ns, so single-op p50 is quantised and only the tail is meaningful. */
    uint64_t floor_ns = UINT64_MAX;
    for (int i = 0; i < 100000; i++) {
        uint64_t a = now_ns(), b = now_ns();
        if (b - a < floor_ns) floor_ns = b - a;
    }

    for (int k = 0; k < reps; k++) {
        for (size_t i = 0; i < n; i++) {
            uint64_t t0 = now_ns();
            ptrs[i] = be_alloc(sizes[i]);
            uint64_t t1 = now_ns();
            lat[m++] = t1 - t0;
            if (ptrs[i]) ((unsigned char *)ptrs[i])[0] = 1;
        }
        be_release(ptrs, n);
    }
    qsort(lat, m, sizeof *lat, cmp_u64);
    emit("latency_mixed", "p50_ns", (double)lat[m / 2]);
    emit("latency_mixed", "p99_ns", (double)lat[m * 99 / 100]);
    emit("latency_mixed", "p999_ns", (double)lat[m * 999 / 1000]);
    emit("latency_mixed", "max_ns", (double)lat[m - 1]);
    emit("latency_mixed", "timer_floor_ns", (double)floor_ns);
    free(lat);
}

/* ---- interleaved alloc/free churn (needs a real free) ------------------- */

#if BE_HAS_FREE
static void run_churn(void) {
    enum { SLOTS = 256, OPS = 400000 };
    static void *slot[SLOTS];
    static size_t slot_sz[SLOTS];
    double s[SAMPLES];
    size_t fails = 0;
    for (int smp = -1; smp < SAMPLES; smp++) {
        memset(slot, 0, sizeof slot);
        uint64_t t0 = now_ns();
        for (int i = 0; i < OPS; i++) {
            size_t k = rnd() % SLOTS;
            if (slot[k]) {
                be_free(slot[k]);
                slot[k] = NULL;
            } else {
                size_t sz = 16 + rnd() % 497;
                slot[k] = be_alloc(sz);
                slot_sz[k] = sz;
                if (!slot[k]) fails++;
                else { ((unsigned char *)slot[k])[0] = 1; ((unsigned char *)slot[k])[sz - 1] = 2; }
            }
        }
        uint64_t t1 = now_ns();
        for (size_t k = 0; k < SLOTS; k++) if (slot[k]) be_free(slot[k]);
        be_release(ptrs, 0);            /* gp: reset_region; malloc: no-op */
        if (smp >= 0) s[smp] = (double)(t1 - t0) / OPS;
    }
    (void)slot_sz;
    emit("churn_alloc_free", "ns_per_op", median(s, SAMPLES));
    emit("churn_alloc_free", "failed_allocs", (double)fails);
}
#endif

/* ---- large allocations: one block per iteration, up to 1 MiB ------------ */

/* 1000 iterations of alloc -> touch -> release for one block size (0 = random
   size in 1..1 MiB-16 per iteration). A bump arena is a single 1 MiB region, so
   1000 live blocks cannot coexist; each block is released before the next, which
   is also how malloc is driven so the comparison stays like-for-like. */
static void run_large(const char *name, size_t fixed) {
    enum { ITERS = 1000 };
    const size_t max_req = 1024 * 1024 - HEADER;
    for (int i = 0; i < ITERS; i++) sizes[i] = fixed ? fixed : 1 + rnd() % max_req;

    double a[SAMPLES], ts[SAMPLES], tf[SAMPLES], r[SAMPLES];
    size_t fails = 0;
    for (int smp = -1; smp < SAMPLES; smp++) {
        uint64_t ta = 0, tts = 0, ttf = 0, tr = 0;
        size_t ok = 0;
        for (int i = 0; i < ITERS; i++) {
            uint64_t t0 = now_ns();
            unsigned char *p = be_alloc(sizes[i]);
            uint64_t t1 = now_ns();
            ta += t1 - t0;
            if (!p) { if (smp >= 0) fails++; continue; }
            ok++;
            p[0] = 1; p[sizes[i] - 1] = 2;
            uint64_t t2 = now_ns();
            memset(p, i & 0xFF, sizes[i]);
            uint64_t t3 = now_ns();
            ptrs[0] = p;
            be_release(ptrs, 1);
            uint64_t t4 = now_ns();
            tts += t2 - t1; ttf += t3 - t2; tr += t4 - t3;
        }
        if (smp < 0 || ok == 0) continue;
        a[smp] = (double)ta / ITERS; ts[smp] = (double)tts / ok;
        tf[smp] = (double)ttf / ok;  r[smp] = (double)tr / ok;
    }
    emit(name, "failed_allocs", (double)fails / SAMPLES);   /* per sample of 1000 */
    if (fails == (size_t)SAMPLES * ITERS) return;           /* unsupported size */
    emit(name, "alloc_ns", median(a, SAMPLES));
    emit(name, "touch_sparse_ns", median(ts, SAMPLES));
    emit(name, "touch_full_ns", median(tf, SAMPLES));
    emit(name, "release_ns", median(r, SAMPLES));
}

/* ---- correctness gate: refuse to benchmark a broken allocator ----------- */

static void self_check(void) {
    size_t n = fill_mixed();
    if (n > 2000) n = 2000;
    for (size_t i = 0; i < n; i++) {
        unsigned char *p = be_alloc(sizes[i]);
        assert(p && ((uintptr_t)p % 8) == 0);
        ptrs[i] = p;
        for (size_t b = 0; b < sizes[i]; b++) p[b] = (unsigned char)(i + b);
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char *p = ptrs[i];
        for (size_t b = 0; b < sizes[i]; b++) assert(p[b] == (unsigned char)(i + b));
    }
    be_release(ptrs, n);
}

int main(void) {
    self_check();
    run_density();

    struct { const char *name; size_t sz; size_t n; int reps; } fixed[] = {
        {"fixed_16B_x100",    16,   100,   2000},
        {"fixed_16B_x1000",   16,  1000,    200},
        {"fixed_16B_x10000",  16, 10000,     20},
        {"fixed_16B_x30000",  16, 30000,      3},
        {"fixed_256B_x2000", 256,  2000,    100},
        {"fixed_4KiB_x150", 4096,   150,   1000},
    };
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++) {
        fill_fixed(fixed[i].n, fixed[i].sz);
        run_batch(fixed[i].name, fixed[i].n, fixed[i].reps);
    }

    size_t n = fill_mixed();
    run_batch("mixed_sizes", n, 150);

    run_large("large_64KiB",      64 * 1024);
    run_large("large_256KiB",    256 * 1024);
    run_large("large_512KiB",    512 * 1024);
    run_large("large_max",  1024 * 1024 - HEADER);   /* largest block afalloc supports */
    run_large("large_1MiB_exact", 1024 * 1024);      /* does not fit with a header */
    run_large("large_random",                0);     /* uniform 1..1 MiB-16 */

    run_latency();
#if BE_HAS_FREE
    run_churn();
#endif
    return 0;
}
