/* Benchmarks for afalloc2 against the system allocator and the old afalloc (mmap/afalloc_persistent.c).
 *
 *   bench <sys|old|af> [quick]      prints "metric,allocator,value" lines
 *
 * run_bench.py runs it once per allocator (and once per afalloc2 security level, because the level is
 * fixed per process) and prints the merged tables. Single-run numbers on a machine that is also doing
 * other work: read the ratios, not the last digit. macOS only (mach timing and RSS).
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../afalloc.h"
/* the old allocator (mmap/afalloc_persistent.c) is compiled with -Dafalloc=old_afalloc -Dafree=old_afree */
extern void *old_afalloc(size_t n);
extern void old_afree(void *p);

typedef void *(*alloc_fn)(size_t);
typedef void (*free_fn)(void *);

static void *old_alloc(size_t n) { return old_afalloc(n); }
static void old_free(void *p) { old_afree(p); }
static void *sys_alloc(size_t n) { return malloc(n); }
static void sys_free(void *p) { free(p); }

static alloc_fn A;
static free_fn F;
static const char *NAME;
static int quick;
static int is_old;

static inline uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void emit(const char *metric, double v) { printf("%s,%s,%.3f\n", metric, NAME, v); fflush(stdout); }

static uint64_t rng_s = 0x9E3779B97F4A7C15ull;
static inline uint64_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return rng_s; }
static volatile uint64_t sink;

/* ---- 1. alloc+free pairs ---- */
static void bench_pairs(void)
{
    static const size_t sizes[] = {16, 64, 256, 1024, 8192, 65536};
    for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        size_t n = sizes[s];
        if (is_old && n > 8192) continue;
        long iters = n <= 1024 ? 10000000 : n <= 8192 ? 4000000 : 100000;
        if (quick) iters /= 10;
        for (long i = 0; i < iters / 10; i++) { char *p = A(n); p[0] = 1; F(p); }      /* warm up */
        uint64_t t0 = now_ns();
        for (long i = 0; i < iters; i++) {
            char *p = A(n);
            p[0] = 1;
            F(p);
        }
        char metric[48];
        snprintf(metric, sizeof metric, "pair_ns_%zu", n);
        emit(metric, (double)(now_ns() - t0) / (double)iters);
    }
}

/* ---- 2. allocate a batch, free it ---- */
static void bench_batch(void)
{
    static const size_t sizes[] = {16, 128, 1024};
    enum { K = 4096 };
    static void *p[K];
    for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        size_t n = sizes[s];
        int rounds = quick ? 200 : 2000;
        uint64_t t0 = now_ns();
        for (int r = 0; r < rounds; r++) {
            for (int i = 0; i < K; i++) { p[i] = A(n); *(char *)p[i] = 1; }
            for (int i = 0; i < K; i++) F(p[i]);
        }
        char metric[48];
        snprintf(metric, sizeof metric, "batch_ns_%zu", n);
        emit(metric, (double)(now_ns() - t0) / ((double)rounds * K));
    }
}

/* ---- 3. mixed churn ---- */
static size_t churn_size(uint64_t r)
{
    unsigned c = (unsigned)(r % 100);
    if (c < 70) return 16 + (size_t)((r >> 8) % 113);
    if (c < 95) return 128 + (size_t)((r >> 8) % 897);
    return 1024 + (size_t)((r >> 8) % 15361);
}

static void bench_churn(void)
{
    enum { SLOTS = 4096 };
    static void *slot[SLOTS];
    memset(slot, 0, sizeof slot);
    long ops = quick ? 1000000 : 10000000;
    for (long i = 0; i < ops / 5; i++) {                       /* warm up */
        unsigned k = (unsigned)(rnd() % SLOTS);
        if (slot[k]) { F(slot[k]); slot[k] = NULL; } else slot[k] = A(churn_size(rnd()));
    }
    uint64_t t0 = now_ns();
    for (long i = 0; i < ops; i++) {
        unsigned k = (unsigned)(rnd() % SLOTS);
        if (slot[k]) {
            F(slot[k]);
            slot[k] = NULL;
        } else {
            char *p = A(churn_size(rnd()));
            p[0] = 1;
            slot[k] = p;
        }
    }
    emit("churn_ns_per_op", (double)(now_ns() - t0) / (double)ops);
    for (int i = 0; i < SLOTS; i++) if (slot[i]) F(slot[i]);
}

/* ---- 4. threads ---- */
typedef struct { atomic_int *go; long iters; size_t size; } tparam_t;
static void *pair_thread(void *arg)
{
    tparam_t *t = arg;
    while (!atomic_load(t->go)) ;
    for (long i = 0; i < t->iters; i++) {
        char *p = A(t->size);
        p[0] = 1;
        F(p);
    }
    return NULL;
}

static void bench_scaling(void)
{
    static const int counts[] = {1, 2, 4, 8};
    for (unsigned c = 0; c < 4; c++) {
        int nt = counts[c];
        long iters = quick ? 500000 : 3000000;
        atomic_int go = 0;
        pthread_t th[8];
        tparam_t tp = { &go, iters, 64 };
        for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, pair_thread, &tp);
        uint64_t t0 = now_ns();
        atomic_store(&go, 1);
        for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
        double secs = (double)(now_ns() - t0) / 1e9;
        char metric[48];
        snprintf(metric, sizeof metric, "mt_Mpairs_per_s_%dthreads", nt);
        emit(metric, (double)nt * (double)iters / secs / 1e6);
    }
}

/* ---- 5. blocks freed by a different thread than the one that allocated them ---- */
#define RING 4096
static void *ring[RING];
static _Atomic unsigned long rhead, rtail;
static long xt_items;

static void *xt_producer(void *arg)
{
    (void)arg;
    for (long i = 0; i < xt_items; i++) {
        char *p = A(64);
        p[0] = 1;
        unsigned long t = atomic_load_explicit(&rtail, memory_order_relaxed);
        while (t - atomic_load_explicit(&rhead, memory_order_acquire) >= RING) ;
        ring[t % RING] = p;
        atomic_store_explicit(&rtail, t + 1, memory_order_release);
    }
    return NULL;
}
static void *xt_consumer(void *arg)
{
    (void)arg;
    for (long i = 0; i < xt_items; i++) {
        unsigned long h = atomic_load_explicit(&rhead, memory_order_relaxed);
        while (atomic_load_explicit(&rtail, memory_order_acquire) == h) ;
        void *p = ring[h % RING];
        atomic_store_explicit(&rhead, h + 1, memory_order_release);
        F(p);
    }
    return NULL;
}

static void bench_cross_thread(void)
{
    xt_items = quick ? 500000 : 4000000;
    atomic_store(&rhead, 0);
    atomic_store(&rtail, 0);
    pthread_t a, b;
    uint64_t t0 = now_ns();
    pthread_create(&a, NULL, xt_producer, NULL);
    pthread_create(&b, NULL, xt_consumer, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    emit("crossthread_ns_per_block", (double)(now_ns() - t0) / (double)xt_items);
}

/* ---- 6. memory footprint under fragmentation ---- */
static size_t rss_bytes(void)
{
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &cnt) != KERN_SUCCESS) return 0;
    return info.resident_size;
}

static void bench_memory(void)
{
    if (is_old) return;
    enum { N = 400000 };
    void **p = calloc(N, sizeof *p);
    size_t *sz = calloc(N, sizeof *sz);
    size_t base = rss_bytes(), live = 0;
    for (int i = 0; i < N; i++) {
        sz[i] = 16 + rnd() % 400;
        p[i] = A(sz[i]);
        memset(p[i], 0xA5, sz[i]);
        live += sz[i];
    }
    size_t full = rss_bytes();
    size_t live_after = 0;
    for (int i = 0; i < N; i++) {
        if (rnd() % 10) { F(p[i]); p[i] = NULL; } else live_after += sz[i];
    }
    size_t after_free = rss_bytes();
    for (int i = 0; i < N; i++) {                           /* allocate the same amount again: is the space reused? */
        if (!p[i]) { sz[i] = 16 + rnd() % 400; p[i] = A(sz[i]); memset(p[i], 0xB6, sz[i]); }
    }
    size_t regrown = rss_bytes();
    emit("mem_live_MB", (double)live / 1048576.0);
    emit("mem_rss_full_MB", (double)(full - base) / 1048576.0);
    emit("mem_rss_after_free90_MB", (double)(after_free > base ? after_free - base : 0) / 1048576.0);
    emit("mem_live_after_free90_MB", (double)live_after / 1048576.0);
    emit("mem_rss_regrown_MB", (double)(regrown - base) / 1048576.0);
    for (int i = 0; i < N; i++) if (p[i]) F(p[i]);
    free(p); free(sz);
}

/* ---- 7. latency of single allocations (clock tick on Apple Silicon is 42 ns: read the tail, not the median) ---- */
static void bench_latency(void)
{
    enum { SLOTS = 4096 };
    static void *slot[SLOTS];
    static uint32_t hist[4096];
    memset(slot, 0, sizeof slot);
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    long ops = quick ? 1000000 : 6000000;
    for (long i = 0; i < ops; i++) {
        unsigned k = (unsigned)(rnd() % SLOTS);
        if (slot[k]) { F(slot[k]); slot[k] = NULL; continue; }
        size_t n = churn_size(rnd());
        uint64_t t0 = mach_absolute_time();
        char *p = A(n);
        uint64_t dt = (mach_absolute_time() - t0) * tb.numer / tb.denom;
        p[0] = 1;
        slot[k] = p;
        hist[dt >= 4095 ? 4095 : dt]++;
    }
    uint64_t total = 0;
    for (int i = 0; i < 4096; i++) total += hist[i];
    double q[] = {0.5, 0.99, 0.999, 0.9999};
    const char *names[] = {"lat_alloc_p50_ns", "lat_alloc_p99_ns", "lat_alloc_p999_ns", "lat_alloc_p9999_ns"};
    uint64_t acc = 0;
    int qi = 0;
    for (int i = 0; i < 4096 && qi < 4; i++) {
        acc += hist[i];
        while (qi < 4 && (double)acc >= q[qi] * (double)total) emit(names[qi++], i);
    }
    for (int i = 0; i < SLOTS; i++) if (slot[i]) F(slot[i]);
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "af";
    quick = argc > 2 && !strcmp(argv[2], "quick");
    if (!strcmp(which, "sys")) { A = sys_alloc; F = sys_free; NAME = "system malloc"; }
    else if (!strcmp(which, "old")) { A = old_alloc; F = old_free; NAME = "afalloc (old)"; is_old = 1; }
    else {
        A = afalloc; F = afree;
        afree(afalloc(1));
        afstats_t st; afget_stats(&st);
        static char nm[40];
        snprintf(nm, sizeof nm, "afalloc2 level %d", st.security_level);
        NAME = nm;
    }
    bench_pairs();
    bench_batch();
    bench_churn();
    bench_scaling();
    bench_cross_thread();
    bench_memory();
    bench_latency();
    return (int)(sink & 0);
}
