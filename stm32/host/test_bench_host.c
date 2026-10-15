/* Runs the benchmark harness on the Mac against the pool and against libc malloc, with a
 * fake cycle counter. This does not measure anything real; it checks that the harness
 * itself is sound (no crash, no out-of-bounds access under the sanitizers, sane counts)
 * before it is trusted on the board. */
#include <stdio.h>
#include <stdlib.h>
#include "bench_common.h"
#include "clock.h"
#include "pool.h"
#include "uart.h"

uint32_t host_cycle_counter;
uint32_t host_cycle_step(void)
{
    static uint32_t s = 12345u;
    s = s * 1664525u + 1013904223u;
    return 2u + (s >> 29);                       /* 2..9 fake cycles per counter read */
}

uint32_t clock_hclk_hz(void)  { return 100000000u; }
uint32_t clock_pclk1_hz(void) { return 50000000u; }

void uart_init(uint32_t pclk1_hz, uint32_t baud) { (void)pclk1_hz; (void)baud; }
void uart_putc(char c) { putchar(c); }
void uart_puts(const char *s) { fputs(s, stdout); }
void uart_put_u32(uint32_t v) { printf("%u", (unsigned)v); }
void uart_put_hex32(uint32_t v) { printf("0x%08x", (unsigned)v); }

int main(void)
{
    pool_init();
    bench_run("pool (host, fake cycle counter)", pool_alloc, pool_free);
    bench_run("libc malloc (host, fake cycle counter)", malloc, free);
    return 0;
}
