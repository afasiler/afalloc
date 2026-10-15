/* Time the pool allocator with the cycle counter and print the result on the serial port.
 * Build with POOL_DEBUG=0 (the Makefile default for bench builds) for the fastest numbers. */
#include "bench_common.h"
#include "board.h"
#include "pool.h"
#include "uart.h"

void app_main(void)
{
    bench_run("pool", pool_alloc, pool_free);

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
    led_set(1);
    for (;;) {
    }
}
