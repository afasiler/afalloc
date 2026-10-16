#include <stdint.h>
#include "bench_common.h"
#include "bench_port.h"    /* supplied by the application: see bench_common.h for the contract */

typedef void *(*alloc_fn)(size_t);
typedef void (*free_fn)(void *);

/* ---- scenario sizes ---- */
#define PAIR_ITERS     2000u    /* alloc+free pairs per size */
#define BATCH          16u      /* blocks alive at once in the batch scenario */
#define BATCH_ROUNDS   200u
#define CHURN_SLOTS    16u      /* live-object table for the random workload */
#define CHURN_OPS      20000u
#define COLD_ITERS     300u     /* operations measured with the code cache emptied first */
#define WARMUP         200u     /* untimed iterations so caches are warm */
#define HIST_BINS      512u     /* cycle counts 0..510 exactly; the last bin means "511 or more" */

typedef void *(*alloc_fn)(size_t);
typedef void (*free_fn)(void *);

/* ---- how the timing window is built --------------------------------------------------
 * By default each call is timed with two plain counter reads around it. The compiler may slide
 * ordinary instructions between a read and the call, and those would be counted as allocator
 * time, so on a real target the port should supply BENCH_TIME_ALLOC / BENCH_TIME_FREE that make
 * the whole window one inline-assembly block (see ports/cortexm/bench_window.h, which makes every
 * call exactly ldr / blx / ldr, and tools/check_bench_windows.py, which proves it on the object
 * file). Without them the numbers are still correct but carry a few cycles of noise. */
#ifndef BENCH_TIME_ALLOC
#define BENCH_TIME_ALLOC(res, fn, arg, t0, t1)  do { (t0) = bench_cycles(); (res) = (fn)(arg); (t1) = bench_cycles(); } while (0)
#define BENCH_TIME_FREE(fn, ptr, t0, t1)        do { (t0) = bench_cycles(); (fn)(ptr); (t1) = bench_cycles(); } while (0)
#endif
#define TIME_ALLOC BENCH_TIME_ALLOC
#define TIME_FREE  BENCH_TIME_FREE

typedef struct {
    uint32_t min, max, n, overhead;
    uint64_t sum;
    uint16_t *hist;
} stat_t;

static uint32_t clamped;        /* samples whose gross time was below the overhead (should be 0) */

static void stat_reset(stat_t *s, uint16_t *hist, uint32_t overhead)
{
    s->min = 0xFFFFFFFFu;
    s->max = 0;
    s->n = 0;
    s->sum = 0;
    s->overhead = overhead;
    s->hist = hist;
    for (unsigned i = 0; i < HIST_BINS; i++)
        hist[i] = 0;
}

static void stat_add(stat_t *s, uint32_t gross)
{
    uint32_t v = 0;
    if (gross >= s->overhead)
        v = gross - s->overhead;
    else
        clamped++;
    if (v < s->min) s->min = v;
    if (v > s->max) s->max = v;
    s->sum += v;
    s->n++;
    s->hist[v < HIST_BINS - 1 ? v : HIST_BINS - 1]++;
}

/* Smallest cycle count c such that at least `num/den` of the samples took <= c. */
static uint32_t percentile(const stat_t *s, uint32_t num, uint32_t den)
{
    uint64_t need = ((uint64_t)s->n * num + den - 1u) / den;     /* ceil */
    if (need == 0)
        need = 1;
    uint64_t seen = 0;
    for (unsigned i = 0; i < HIST_BINS; i++) {
        seen += s->hist[i];
        if (seen >= need)
            return i;
    }
    return HIST_BINS - 1;
}

static uint32_t g_rng = 88172645u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void put_padded(uint32_t v, unsigned width)
{
    char buf[11];
    unsigned n = 0;
    do { buf[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    for (unsigned i = n; i < width; i++)
        bench_putc(' ');
    while (n)
        bench_putc(buf[--n]);
}

/* value given in tenths, printed as "12.3", right-aligned in `width` columns */
static void put_tenths(uint64_t tenths, unsigned width)
{
    uint32_t whole = (uint32_t)(tenths / 10u), frac = (uint32_t)(tenths % 10u);
    unsigned digits = 1;
    for (uint32_t w = whole; w >= 10u; w /= 10u)
        digits++;
    for (unsigned i = digits + 2; i < width; i++)
        bench_putc(' ');
    bench_put_u32(whole);
    bench_putc('.');
    bench_putc((char)('0' + frac));
}

#define LABEL_WIDTH 32u
static void put_label(const char *label)
{
    unsigned n = 0;
    while (label[n]) {
        bench_putc(label[n]);
        n++;
    }
    for (; n < LABEL_WIDTH; n++)
        bench_putc(' ');
}

static void report(const char *label, const stat_t *s)
{
    put_label(label);
    if (s->n == 0) {
        bench_puts("   (no samples)\n");
        return;
    }
    uint64_t avg10 = (s->sum * 10u) / s->n;
    uint64_t ns10 = (s->sum * 10000000000ull) / ((uint64_t)s->n * bench_clock_hz());
    put_padded(s->n, 6);
    put_padded(s->min, 5);
    put_padded(percentile(s, 1, 2), 5);
    put_tenths(avg10, 7);
    put_padded(percentile(s, 99, 100), 5);
    put_padded(percentile(s, 999, 1000), 6);
    put_padded(s->max, 6);
    put_tenths(ns10, 9);
    bench_puts("\n");
}

/* ---- calibration: what the measuring itself costs ----------------------------------------
 * An empty function called through the same kind of function pointer, timed by the same
 * macros as the real calls. Whatever it costs (the counter reads, the indirect call, the
 * return) is not the allocator's, so it is subtracted. Using the minimum of many runs gives
 * the cheapest possible overhead, which under-subtracts rather than over-subtracts. */
static void *nop_alloc(size_t n) __attribute__((noinline));
static void nop_free(void *p) __attribute__((noinline));
static void *nop_alloc(size_t n) { return (void *)n; }
static void nop_free(void *p) { (void)p; }

static uint32_t overhead_alloc, overhead_free;

static void calibrate(void)
{
    alloc_fn volatile af = nop_alloc;
    free_fn volatile ff = nop_free;
    uint32_t best_a = 0xFFFFFFFFu, best_f = 0xFFFFFFFFu;
    for (unsigned i = 0; i < 2000; i++) {
        uint32_t t0, t1;
        void *r;
        TIME_ALLOC(r, af, 64u, t0, t1);
        if (t1 - t0 < best_a) best_a = t1 - t0;
        TIME_FREE(ff, r, t0, t1);
        if (t1 - t0 < best_f) best_f = t1 - t0;
    }
    overhead_alloc = best_a;
    overhead_free = best_f;
}

/* Empty any instruction/data cache that sits in front of the code memory, so the "cold" scenario
   measures the allocator coming from slow memory. The port defines BENCH_CACHE_FLUSH(); without
   one there is nothing to flush (cores with no cache, or a computer) and cold == warm. */
#ifndef BENCH_CACHE_FLUSH
#define BENCH_CACHE_FLUSH() ((void)0)
#endif
static void cache_flush(void) { BENCH_CACHE_FLUSH(); }

static uint16_t hist_a[HIST_BINS], hist_f[HIST_BINS];

/* alloc+free of one block, many times: the best case (hot free list). */
static void scenario_pair(alloc_fn alloc, free_fn release, size_t size, const char *la, const char *lf)
{
    stat_t sa, sf;
    stat_reset(&sa, hist_a, overhead_alloc);
    stat_reset(&sf, hist_f, overhead_free);
    uint32_t failed = 0;
    for (unsigned i = 0; i < WARMUP + PAIR_ITERS; i++) {
        uint32_t t0, t1, t2, t3;
        void *p;
        TIME_ALLOC(p, alloc, size, t0, t1);
        if (p == 0) {
            if (i >= WARMUP) failed++;
            continue;
        }
        TIME_FREE(release, p, t2, t3);
        if (i >= WARMUP) {
            stat_add(&sa, t1 - t0);
            stat_add(&sf, t3 - t2);
        }
    }
    report(la, &sa);
    report(lf, &sf);
    if (failed) {
        bench_puts("  pair allocation failures: ");
        bench_put_u32(failed);
        bench_puts("\n");
    }
}

/* The same pair, but the code cache is emptied before each call: what the allocator costs
 * when the surrounding code has pushed it out of the cache. */
static void scenario_cold(alloc_fn alloc, free_fn release, size_t size, const char *la, const char *lf)
{
    stat_t sa, sf;
    stat_reset(&sa, hist_a, overhead_alloc);
    stat_reset(&sf, hist_f, overhead_free);
    for (unsigned i = 0; i < COLD_ITERS; i++) {
        uint32_t t0, t1, t2, t3;
        void *p;
        cache_flush();
        TIME_ALLOC(p, alloc, size, t0, t1);
        if (p == 0)
            continue;
        cache_flush();
        TIME_FREE(release, p, t2, t3);
        stat_add(&sa, t1 - t0);
        stat_add(&sf, t3 - t2);
    }
    report(la, &sa);
    report(lf, &sf);
}

/* A burst of allocations followed by freeing them all. */
static void scenario_batch(alloc_fn alloc, free_fn release)
{
    static void *held[BATCH];
    stat_t sa, sf;
    uint32_t failed = 0;
    stat_reset(&sa, hist_a, overhead_alloc);
    stat_reset(&sf, hist_f, overhead_free);
    const unsigned warm_rounds = WARMUP / 10;
    for (unsigned r = 0; r < warm_rounds + BATCH_ROUNDS; r++) {
        for (unsigned k = 0; k < BATCH; k++) {
            uint32_t t0, t1;
            void *p;
            TIME_ALLOC(p, alloc, 64u, t0, t1);
            held[k] = p;
            if (r >= warm_rounds) {
                if (p) stat_add(&sa, t1 - t0); else failed++;
            }
        }
        for (unsigned k = 0; k < BATCH; k++) {
            if (!held[k]) continue;
            uint32_t t0, t1;
            TIME_FREE(release, held[k], t0, t1);
            if (r >= warm_rounds)
                stat_add(&sf, t1 - t0);
            held[k] = 0;
        }
    }
    report("batch alloc 64 B", &sa);
    report("batch free  64 B", &sf);
    if (failed) {
        bench_puts("  batch allocation failures: ");
        bench_put_u32(failed);
        bench_puts("\n");
    }
}

/* Random mixed-size workload on a small table of live objects: closest to real use. */
static void scenario_churn(alloc_fn alloc, free_fn release)
{
    static void *live[CHURN_SLOTS];
    stat_t sa, sf;
    uint32_t failed = 0, requested = 0;
    uint64_t bytes = 0;
    stat_reset(&sa, hist_a, overhead_alloc);
    stat_reset(&sf, hist_f, overhead_free);
    for (unsigned i = 0; i < CHURN_SLOTS; i++)
        live[i] = 0;
    g_rng = 88172645u;                          /* same sequence for every allocator */

    for (unsigned op = 0; op < WARMUP + CHURN_OPS; op++) {
        unsigned k = rnd() % CHURN_SLOTS;
        uint32_t t0, t1;
        if (live[k]) {
            TIME_FREE(release, live[k], t0, t1);
            live[k] = 0;
            if (op >= WARMUP) stat_add(&sf, t1 - t0);
        } else {
            size_t size = 8u + rnd() % 249u;     /* 8..256 bytes */
            void *p;
            TIME_ALLOC(p, alloc, size, t0, t1);
            live[k] = p;
            if (op >= WARMUP) {
                requested++;
                bytes += size;
                if (p) stat_add(&sa, t1 - t0); else failed++;
            }
        }
    }
    for (unsigned i = 0; i < CHURN_SLOTS; i++)
        if (live[i]) release(live[i]);

    report("random alloc 8..256 B", &sa);
    report("random free", &sf);
    bench_puts("  random workload: ");
    bench_put_u32(requested);
    bench_puts(" allocation requests, ");
    bench_put_u32((uint32_t)bytes);
    bench_puts(" bytes requested (must match across allocators), failures: ");
    bench_put_u32(failed);
    bench_puts("\n");
}

void bench_run(const char *name, bench_alloc_fn alloc, bench_free_fn release)
{
    clamped = 0;
    calibrate();

    bench_puts("\n--- benchmark: ");
    bench_puts(name);
    bench_puts(" ---\n");
#ifdef BUILD_INFO
    bench_puts("build: " BUILD_INFO " ; gcc " __VERSION__ "\n");
#endif
    bench_puts("CPU clock ");
    bench_put_u32(bench_clock_hz());
    bench_puts(" Hz");
    if (bench_clock_hz() != bench_target_hz()) {
        bench_puts("  *** WARNING: not at the target of ");
        bench_put_u32(bench_target_hz());
        bench_puts(" Hz: the clock setup failed, numbers are for the clock actually running ***");
    }
    bench_puts("\nmeasuring cost subtracted: alloc ");
    bench_put_u32(overhead_alloc);
    bench_puts(" cycles, free ");
    bench_put_u32(overhead_free);
    bench_puts(" cycles (an empty call through a function pointer, timed the same way)\n");
    put_label("");
    bench_puts(" count  min  p50    avg  p99 p99.9   max   avg ns   (net cycles per call)\n");

    scenario_pair(alloc, release, 16,  "pair  alloc 16 B", "pair  free  16 B");
    scenario_pair(alloc, release, 64,  "pair  alloc 64 B", "pair  free  64 B");
    scenario_pair(alloc, release, 256, "pair  alloc 256 B", "pair  free  256 B");
    scenario_batch(alloc, release);
    scenario_churn(alloc, release);
    scenario_cold(alloc, release, 64, "COLD  alloc 64 B", "COLD  free  64 B");

    if (clamped) {
        bench_puts("note: ");
        bench_put_u32(clamped);
        bench_puts(" samples were shorter than the calibrated overhead and were counted as 0\n");
    }
    bench_puts("--- done ---\n");
}
