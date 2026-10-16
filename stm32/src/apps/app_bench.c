/* Time the pool allocator with the cycle counter and print the result on the serial port.
 * Build with POOL_DEBUG=0 (the Makefile default for bench builds) for the fastest numbers. */
#include "bench_common.h"
#include "bench_port.h"
#include "board.h"
#include "pool.h"
#include "uart.h"

/* Average bytes lost to rounding up, for requests spread evenly over 8..256 bytes. */
static void report_waste(void)
{
    uint64_t waste = 0, asked = 0;
    for (uint32_t size = 8; size <= 256; size++) {
        for (unsigned i = 0; i < pool_count(); i++) {
            pool_stats_t s;
            pool_get_stats(i, &s);
            if (size <= s.block_size) {
                waste += s.block_size - size;
                break;
            }
        }
        asked += size;
    }
    uart_puts("memory: pools hold ");
    uart_put_u32(pool_total_bytes());
    uart_puts(" bytes in total (static, reserved up front); requests of 8..256 B waste on average ");
    uart_put_u32((uint32_t)(waste * 100u / asked));
    uart_puts("% to rounding up, and there is no per-block header\n");
}

/* ---- fixed-size allocation: pool_alloc(n) against POOL_ALLOC(n) ----
 * Same timing windows and the same alloc+free loop for both, so the only difference is how the
 * pool is found: by a run-time size (clz + table) or by a class index the compiler computed. */
static void *__attribute__((noinline)) empty_alloc(size_t n) { (void)n; return 0; }
static void __attribute__((noinline)) empty_free(void *p) { (void)p; }

typedef struct { uint32_t amin, amax, fmin, fmax; } minmax_t;

static minmax_t measure(void *(*afn)(size_t), size_t arg, void (*ffn)(void *))
{
    minmax_t m = { 0xFFFFFFFFu, 0, 0xFFFFFFFFu, 0 };
    for (unsigned i = 0; i < 2200; i++) {
        void *r;
        uint32_t t0, t1;
        BENCH_TIME_ALLOC(r, afn, arg, t0, t1);
        uint32_t da = t1 - t0;
        BENCH_TIME_FREE(ffn, r, t0, t1);
        uint32_t df = t1 - t0;
        if (i < 200)
            continue;                                     /* warm-up */
        if (da < m.amin) m.amin = da;
        if (da > m.amax) m.amax = da;
        if (df < m.fmin) m.fmin = df;
        if (df > m.fmax) m.fmax = df;
    }
    return m;
}

static void print_range(const char *label, uint32_t lo, uint32_t hi, uint32_t overhead)
{
    uart_puts(label);
    uart_put_u32(lo - overhead);
    if (hi != lo) {
        uart_puts("..");
        uart_put_u32(hi - overhead);
    }
}

static void report_fixed(size_t size, size_t cls, minmax_t ov)
{
    minmax_t a = measure(pool_alloc, size, pool_free);
    minmax_t b = measure(pool_alloc_class, cls, pool_free);
    uart_puts("  ");
    uart_put_u32((uint32_t)size);
    uart_puts(" B   alloc: pool_alloc ");
    print_range("", a.amin, a.amax, ov.amin);
    uart_puts("  POOL_ALLOC ");
    print_range("", b.amin, b.amax, ov.amin);
    uart_puts("   free: ");
    print_range("", b.fmin, b.fmax, ov.fmin);
    uart_puts("\n");
}

static void fixed_size_section(void)
{
    uart_puts("\n--- fixed-size allocation (net cycles per call, min..max over 2000 calls) ---\n");
    minmax_t ov = measure(empty_alloc, 0, empty_free);
    /* The sizes are constants here on purpose: POOL_ALLOC needs them at compile time. */
    report_fixed(16, pool_class_of(16), ov);
    report_fixed(64, pool_class_of(64), ov);
    report_fixed(256, pool_class_of(256), ov);
    uart_puts("--- done ---\n");
}

void app_main(void)
{
    bench_run("pool", pool_alloc, pool_free);
    report_waste();
    fixed_size_section();

    pool_stats_t s;
    uart_puts("pool statistics after the run (peak in use / blocks, failed allocs):\n");
    for (unsigned i = 0; i < pool_count(); i++) {
        pool_get_stats(i, &s);
        uart_puts("  ");
        uart_put_u32(s.block_size);
        uart_puts(" B: ");
        uart_put_u32(s.peak);
        uart_puts(" / ");
        uart_put_u32(s.count);
        uart_puts(", failed ");
        uart_put_u32(s.alloc_fail);
        uart_puts("\n");
    }
    uart_flush();
    led_set(1);
    for (;;) {
    }
}
