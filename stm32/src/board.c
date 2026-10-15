#include "board.h"
#include "clock.h"
#include "dwt.h"
#include "regs.h"

#define LED_PIN 5u

void board_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    (void)RCC->AHB1ENR;                                   /* let the clock enable settle */

    /* MODER holds 2 bits per pin. Change only PA5's two bits: PA13/PA14 carry the debug
       interface (SWD) and must stay in alternate-function mode. */
    GPIOA->MODER = (GPIOA->MODER & ~(0x3u << (LED_PIN * 2u))) | (0x1u << (LED_PIN * 2u));
    led_set(0);
}

void led_set(int on)
{
    /* BSRR: low half sets a pin, high half resets it. One atomic write, no read needed. */
    GPIOA->BSRR = on ? (1u << LED_PIN) : (1u << (LED_PIN + 16u));
}

void led_toggle(void)
{
    led_set((GPIOA->ODR & (1u << LED_PIN)) == 0);
}

void delay_cycles(uint32_t n)
{
    uint32_t start = cycles();
    while ((uint32_t)(cycles() - start) < n) {
    }
}

void delay_ms(uint32_t ms)
{
    delay_cycles(ms * (clock_hclk_hz() / 1000u));
}

void led_blink(unsigned count, uint32_t on_ms, uint32_t off_ms)
{
    for (unsigned i = 0; i < count; i++) {
        led_set(1);
        delay_ms(on_ms);
        led_set(0);
        delay_ms(off_ms);
    }
    delay_ms(600);
}
