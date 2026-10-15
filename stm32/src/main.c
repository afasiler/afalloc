/* Program entry: bring the board up in small, observable steps, then hand over to the
 * selected application (apps/app_<name>.c, chosen with `make APP=<name>`).
 *
 * The LED reports progress even if the serial output is not connected:
 *   1 blink                 the program started and the cycle counter works
 *   2 blinks                the PLL clock is running (CLOCK_MHZ, default 96 MHz)
 *   5 fast blinks           the PLL setup failed: continuing at 16 MHz (see the serial log)
 *   endless fast triple blink  this core has no cycle counter (cannot continue)
 */
#include "board.h"
#include "clock.h"
#include "dwt.h"
#include "pool.h"
#include "uart.h"

#ifndef APP_NAME
#define APP_NAME "app"
#endif

void app_main(void);          /* implemented by exactly one file in apps/ */

/* Used only when the cycle counter is missing, so it cannot rely on it. */
static void panic_blink(void)
{
    for (;;) {
        for (int i = 0; i < 3; i++) {
            led_set(1);
            for (volatile uint32_t d = 0; d < 300000u; d++) {}
            led_set(0);
            for (volatile uint32_t d = 0; d < 300000u; d++) {}
        }
        for (volatile uint32_t d = 0; d < 2000000u; d++) {}
    }
}

static void print_kv(const char *key, uint32_t value, const char *unit)
{
    uart_puts(key);
    uart_put_u32(value);
    uart_puts(unit);
}

int main(void)
{
    board_init();
    if (dwt_init() != 0)
        panic_blink();

    led_blink(1, 250, 250);                       /* stage 1: alive, still at 16 MHz */

    int clock_rc = clock_init_pll();
    if (clock_rc == 0)
        led_blink(2, 250, 250);                   /* stage 2: PLL clock running */
    else
        led_blink(5, 80, 80);                     /* PLL problem: still running at 16 MHz */

    uart_init(clock_pclk1_hz(), 115200);
    uart_puts("\n\n=== afalloc stm32 : " APP_NAME " ===\n");
    print_kv("HCLK  = ", clock_hclk_hz(), " Hz\n");
    print_kv("PCLK1 = ", clock_pclk1_hz(), " Hz\n");
    if (clock_rc != 0)
        print_kv("clock_init_pll failed at stage ", (uint32_t)clock_rc, " (running at 16 MHz)\n");

    if (pool_init() != 0) {
        /* A bad pool table is a programming error: stop here and say so, do not run on. */
        uart_puts("pool_init: invalid pool configuration (sizes must ascend)! halting\n");
        uart_flush();
        for (;;) {
            led_toggle();
            delay_ms(80);
        }
    }
    print_kv("pools = ", pool_count(), "");
    print_kv(", block memory = ", pool_total_bytes(), " bytes\n");

    app_main();

    for (;;) {                                    /* app_main must not return */
    }
}
