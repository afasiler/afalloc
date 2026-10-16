/* What the portable benchmark harness (embedded_pool/src/bench_common.c) needs from this board:
 * the DWT cycle counter, the serial port, the clock, and the ART cache flush. */
#ifndef BENCH_PORT_H
#define BENCH_PORT_H

#include <stdint.h>
#include "clock.h"
#include "dwt.h"
#include "regs.h"
#include "uart.h"
#include "bench_window.h"       /* tight ldr/blx/ldr timing windows for Cortex-M (embedded_pool/ports/cortexm) */

static inline uint32_t bench_cycles(void) { return cycles(); }
static inline void bench_putc(char c) { uart_putc(c); }
static inline void bench_puts(const char *s) { uart_puts(s); }
static inline void bench_put_u32(uint32_t v) { uart_put_u32(v); }
static inline uint32_t bench_clock_hz(void) { return clock_hclk_hz(); }
static inline uint32_t bench_target_hz(void) { return clock_target_hz(); }

/* Empty the ART's instruction and data caches (RM0383 p.59: they can only be reset while
 * disabled). The prefetch line already fetched cannot be flushed, so this is "cold" in the
 * sense that matters: the allocator code and data come from the flash again. */
static inline void bench_cache_flush(void)
{
    uint32_t acr = FLASH->ACR;
    FLASH->ACR = acr & ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    FLASH->ACR |= FLASH_ACR_ICRST | FLASH_ACR_DCRST;
    FLASH->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH->ACR = acr;
    __asm volatile("dsb\n\tisb" : : : "memory");
}
#define BENCH_CACHE_FLUSH() bench_cache_flush()

#endif
