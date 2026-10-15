/* Nucleo-F411RE board support: the user LED (LD2 = PA5, confirmed on the board:
 * driving PA5 high lights it) and cycle-counter based delays. */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

void board_init(void);                 /* clock GPIOA, make PA5 an output, LED off */
void led_set(int on);
void led_toggle(void);

void delay_cycles(uint32_t n);         /* busy-wait using the DWT cycle counter */
void delay_ms(uint32_t ms);            /* uses the current CPU clock */

/* Blink `count` times, `on_ms` on and `off_ms` off, then a longer pause. Used to
 * report progress and errors when there is no other output yet. */
void led_blink(unsigned count, uint32_t on_ms, uint32_t off_ms);

#endif
