/* The same benchmark on newlib-nano's malloc/free, for comparison with app_bench. Needs the
 * C library: the Makefile links it for this app only, with the heap taken from the
 * remaining RAM by sbrk.c. */
#include <stdlib.h>
#include "bench_common.h"
#include "board.h"
#include "uart.h"

void app_main(void)
{
    bench_run("newlib-nano malloc", malloc, free);
    led_set(1);
    for (;;) {
    }
}
