#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

/* After reset the chip runs from the 16 MHz internal oscillator (HSI), no PLL.
 *
 * CLOCK_MHZ (build option, default 96) is the system clock the PLL produces from the HSI:
 *   96  safe default: about 4 % below the chip's 100 MHz limit, because the HSI is only
 *       accurate to +-1 % at 25 C and up to +5.5 % hot (DS10314 Table 39, p.87), and the
 *       PLL multiplies that error. Also gives exactly 48 MHz on the PLL's second output.
 *  100  the chip's maximum; with the HSI error the real clock can exceed the limit.
 */
#ifndef CLOCK_MHZ
#define CLOCK_MHZ 96
#endif

/* Switch the CPU to CLOCK_MHZ: voltage scale, flash wait states and caches, PLL, bus
 * dividers. Safe to call when the PLL is already running (it returns to the HSI first).
 * Returns 0 on success. On failure the chip is put back on the 16 MHz HSI and a positive
 * stage number is returned so the program can still report the problem:
 *   1 flash wait states did not take effect   2 PLL did not lock
 *   3 clock switch to the PLL failed          4 voltage regulator not ready
 *   5 could not return to the HSI (the clock state is then unknown) */
int clock_init_pll(void);

uint32_t clock_hclk_hz(void);     /* current CPU / AHB clock */
uint32_t clock_pclk1_hz(void);    /* current APB1 clock (USART2, TIM2..5, PWR) */
uint32_t clock_target_hz(void);   /* what clock_init_pll() aims for (CLOCK_MHZ) */

#endif
