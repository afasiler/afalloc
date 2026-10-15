#include <stdint.h>
#include "bench_common.h"
#include "clock.h"
#include "dwt.h"
#include "uart.h"

/* ---- scenario sizes ---- */
#define PAIR_ITERS     2000u    /* alloc+free pairs per size */
#define BATCH          16u      /* blocks alive at once in the batch scenario */
#define BATCH_ROUNDS   200u
#define CHURN_SLOTS    16u      /* live-object table for the random workload */
#define CHURN_OPS      20000u
#define WARMUP         200u     /* untimed iterations so caches and the ART are warm */

typedef struct {
    uint32_t min, max, n;
    uint64_t sum;
} stat_t;

static uint32_t overhead;       /* cycles for two back-to-back counter reads */

static void stat_reset(stat_t *s) { s->min = 0xFFFFFFFFu; s->max = 0; s->n = 0; s->sum = 0; }

static void stat_add(stat_t *s, uint32_t cycles_taken)
{
    uint32_t v = cycles_taken > overhead ? cycles_taken - overhead : 0;
    if (v < s->min) s->min = v;
    if (v > s->max) s->max = v;
    s->sum += v;
    s->n++;
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
        uart_putc(' ');
    while (n)
        uart_putc(buf[--n]);
}

static void report(const char *label, const stat_t *s)
{
    uart_puts(label);
    uint32_t avg = s->n ? (uint32_t)(s->sum / s->n) : 0;
    uint32_t ns = (uint32_t)(((uint64_t)avg * 1000000000ull) / clock_hclk_hz());
    put_padded(s->n, 7);
    put_padded(s->n ? s->min : 0, 7);
    put_padded(avg, 7);
    put_padded(s->max, 7);
    put_padded(ns, 8);
    uart_puts("\n");
}

static void measure_overhead(void)
{
    uint32_t best = 0xFFFFFFFFu;
    for (unsigned i = 0; i < 1000; i++) {
        uint32_t a = cycles();
        uint32_t b = cycles();
        if (b - a < best)
            best = b - a;
    }
    overhead = best;
}

/* alloc+free of one block, many times: the best case (hot free list). */
static void scenario_pair(bench_alloc_fn alloc, bench_free_fn release, size_t size, const char *la, const char *lf)
{
    stat_t sa, sf;
    stat_reset(&sa);
    stat_reset(&sf);
    for (unsigned i = 0; i < WARMUP + PAIR_ITERS; i++) {
        uint32_t t0 = cycles();
        void *p = alloc(size);
        uint32_t t1 = cycles();
        if (p) {
            uint32_t t2 = cycles();
            release(p);
            uint32_t t3 = cycles();
            if (i >= WARMUP) {
                stat_add(&sa, t1 - t0);
                stat_add(&sf, t3 - t2);
            }
        }
    }
    report(la, &sa);
    report(lf, &sf);
}

/* A burst of allocations followed by freeing them all. */
static void scenario_batch(bench_alloc_fn alloc, bench_free_fn release)
{
    static void *held[BATCH];
    stat_t sa, sf;
    uint32_t failed = 0;
    stat_reset(&sa);
    stat_reset(&sf);
    for (unsigned r = 0; r < WARMUP / 10 + BATCH_ROUNDS; r++) {
        for (unsigned k = 0; k < BATCH; k++) {
            uint32_t t0 = cycles();
            held[k] = alloc(64);
            uint32_t t1 = cycles();
            if (r >= WARMUP / 10) {
                if (held[k]) stat_add(&sa, t1 - t0); else failed++;
            }
        }
        for (unsigned k = 0; k < BATCH; k++) {
            if (!held[k]) continue;
            uint32_t t0 = cycles();
            release(held[k]);
            uint32_t t1 = cycles();
            if (r >= WARMUP / 10)
                stat_add(&sf, t1 - t0);
            held[k] = 0;
        }
    }
    report("batch alloc 64 B           ", &sa);
    report("batch free  64 B           ", &sf);
    if (failed) {
        uart_puts("  batch allocation failures: ");
        uart_put_u32(failed);
        uart_puts("\n");
    }
}

/* Random mixed-size workload on a small table of live objects: closest to real use. */
static void scenario_churn(bench_alloc_fn alloc, bench_free_fn release)
{
    static void *live[CHURN_SLOTS];
    stat_t sa, sf;
    uint32_t failed = 0;
    stat_reset(&sa);
    stat_reset(&sf);
    for (unsigned i = 0; i < CHURN_SLOTS; i++)
        live[i] = 0;
    g_rng = 88172645u;                          /* same sequence for every allocator */

    for (unsigned op = 0; op < WARMUP + CHURN_OPS; op++) {
        unsigned k = rnd() % CHURN_SLOTS;
        if (live[k]) {
            uint32_t t0 = cycles();
            release(live[k]);
            uint32_t t1 = cycles();
            live[k] = 0;
            if (op >= WARMUP) stat_add(&sf, t1 - t0);
        } else {
            size_t size = 8u + rnd() % 249u;     /* 8..256 bytes */
            uint32_t t0 = cycles();
            void *p = alloc(size);
            uint32_t t1 = cycles();
            live[k] = p;
            if (op >= WARMUP) { if (p) stat_add(&sa, t1 - t0); else failed++; }
        }
    }
    for (unsigned i = 0; i < CHURN_SLOTS; i++)
        if (live[i]) release(live[i]);

    report("random alloc 8..256 B      ", &sa);
    report("random free                ", &sf);
    uart_puts("  random-workload allocation failures: ");
    uart_put_u32(failed);
    uart_puts("\n");
}

void bench_run(const char *name, bench_alloc_fn alloc, bench_free_fn release)
{
    measure_overhead();

    uart_puts("\n--- benchmark: ");
    uart_puts(name);
    uart_puts(" ---\n");
    uart_puts("CPU clock ");
    uart_put_u32(clock_hclk_hz());
    uart_puts(" Hz, counter-read overhead ");
    uart_put_u32(overhead);
    uart_puts(" cycles (already subtracted)\n");
    uart_puts("                                 count    min    avg    max  avg ns   (cycles per call)\n");

    scenario_pair(alloc, release, 16,  "pair  alloc 16 B           ", "pair  free  16 B           ");
    scenario_pair(alloc, release, 64,  "pair  alloc 64 B           ", "pair  free  64 B           ");
    scenario_pair(alloc, release, 256, "pair  alloc 256 B          ", "pair  free  256 B          ");
    scenario_batch(alloc, release);
    scenario_churn(alloc, release);

    uart_puts("--- done ---\n");
}
