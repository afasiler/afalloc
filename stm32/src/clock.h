#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

/* After reset the chip runs from the 16 MHz internal oscillator (HSI), no PLL. */

/* Switch to 100 MHz: HSI -> PLL, flash wait states, voltage scale, bus dividers.
 * Returns 0 on success. On failure returns a positive stage number and the chip
 * keeps running at 16 MHz, so the program can still report the problem:
 *   1 flash latency did not take effect, 2 PLL did not lock, 3 clock switch failed. */
int clock_init_100mhz(void);

uint32_t clock_hclk_hz(void);    /* CPU / AHB clock */
uint32_t clock_pclk1_hz(void);   /* APB1 clock (USART2, TIM2..5, PWR) */

#endif
