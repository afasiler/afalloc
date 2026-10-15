#include "uart.h"
#include "regs.h"

#define TX_PIN 2u
#define RX_PIN 3u
#define AF_USART2 7u

void uart_init(uint32_t pclk1_hz, uint32_t baud)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    (void)RCC->APB1ENR;

    /* PA2/PA3 -> alternate function 7 (AFRL, 4 bits per pin) FIRST, then alternate-function
       mode (MODER = 10), so the pins never sit in another function in between. */
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~((0xFu << (TX_PIN * 4u)) | (0xFu << (RX_PIN * 4u))))
                  | (AF_USART2 << (TX_PIN * 4u)) | (AF_USART2 << (RX_PIN * 4u));
    GPIOA->MODER = (GPIOA->MODER & ~((0x3u << (TX_PIN * 2u)) | (0x3u << (RX_PIN * 2u))))
                 | (0x2u << (TX_PIN * 2u)) | (0x2u << (RX_PIN * 2u));

    /* With 16x oversampling (OVER8 = 0) USARTDIV = fck / (16 * baud), and BRR holds
       USARTDIV as 12.4 fixed point, i.e. USARTDIV * 16 = fck / baud. RM0383 19.3.4. */
    USART2->BRR = (pclk1_hz + baud / 2u) / baud;
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

void uart_putc(char c)
{
    while ((USART2->SR & USART_SR_TXE) == 0) {
    }
    USART2->DR = (uint32_t)(unsigned char)c;
}

void uart_flush(void)
{
    /* TXE only says the byte moved to the shift register; TC says it has left the pin. */
    while ((USART2->SR & USART_SR_TC) == 0) {
    }
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_put_u32(uint32_t v)
{
    char buf[11];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        uart_putc(buf[--n]);
}

void uart_put_hex32(uint32_t v)
{
    uart_putc('0');
    uart_putc('x');
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint32_t d = (v >> shift) & 0xFu;
        uart_putc((char)(d < 10 ? '0' + d : 'a' + (d - 10)));
    }
}
