/* The same benchmark on newlib-nano's malloc/free, for comparison with app_bench. Needs the
 * C library: the Makefile links it for this app only, with the heap taken from the
 * remaining RAM by sbrk.c. Note this library is a prebuilt -Os build and its lock hooks are
 * empty stubs here, so it is NOT interrupt-safe, unlike the pool's default. */
#include <stddef.h>
#include <stdlib.h>
#include "bench_common.h"
#include "board.h"
#include "uart.h"

extern char _sheap;
void *_sbrk(ptrdiff_t increment);

void app_main(void)
{
    bench_run("newlib-nano malloc", malloc, free);

    uart_puts("memory: the heap grew to ");
    uart_put_u32((uint32_t)((char *)_sbrk(0) - &_sheap));
    uart_puts(" bytes (its high-water mark: nano malloc never returns memory); each block also carries a header\n");
    uart_flush();
    led_set(1);
    for (;;) {
    }
}
