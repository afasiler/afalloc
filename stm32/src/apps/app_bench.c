/* Time the pool allocator with the cycle counter and print the result on the serial port.
 * Build with POOL_DEBUG=0 (the Makefile default for bench builds) for the fastest numbers. */
#include "bench_common.h"
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

void app_main(void)
{
    bench_run("pool", pool_alloc, pool_free);
    report_waste();

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
