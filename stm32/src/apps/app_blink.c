/* First bring-up application: blink LD2 once a second and print a counter on the serial
 * port. If the LED blinks and the counter appears, the toolchain, linker script, startup
 * code, clock, GPIO and UART all work. */
#include "board.h"
#include "uart.h"

void app_main(void)
{
    uint32_t n = 0;
    for (;;) {
        led_set(1);
        delay_ms(500);
        led_set(0);
        delay_ms(500);
        uart_puts("tick ");
        uart_put_u32(++n);
        uart_puts("\n");
    }
}
