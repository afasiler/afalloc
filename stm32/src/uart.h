/* Polled transmit on USART2 (PA2 = TX, PA3 = RX, alternate function 7: DS10314
 * Table 9, p.48). On the Nucleo board these pins go to the ST-Link, which shows
 * up on the Mac as a serial port: `screen /dev/cu.usbmodem* 115200`. */
#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(uint32_t pclk1_hz, uint32_t baud);
void uart_putc(char c);
void uart_flush(void);               /* wait until the last byte has left the pin */
void uart_puts(const char *s);
void uart_put_u32(uint32_t v);        /* decimal */
void uart_put_hex32(uint32_t v);      /* 0x%08x */

#endif
