/* Run the pool self-test on the board and report the result on the serial port and the
 * LED: steady on = every test passed, fast blinking = at least one failed. */
#include "board.h"
#include "pool_selftest.h"
#include "uart.h"

static void log_line(const char *name, int failed, int line)
{
    uart_puts(failed ? "  FAIL  " : "  ok    ");
    uart_puts(name);
    if (failed) {
        uart_puts(" (check at pool_selftest.c:");
        uart_put_u32((uint32_t)line);
        uart_puts(")");
    }
    uart_puts("\n");
}

void app_main(void)
{
    uart_puts("running pool self-test...\n");
    int failures = pool_selftest(log_line);

    if (failures == 0) {
        uart_puts("SELFTEST PASSED\n");
        led_set(1);
        for (;;) {
        }
    }
    uart_put_u32((uint32_t)failures);
    uart_puts(" test(s) FAILED\n");
    for (;;) {
        led_toggle();
        delay_ms(100);
    }
}
