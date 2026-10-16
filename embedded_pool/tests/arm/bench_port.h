/* A bench_port.h used only to compile-check the harness for Cortex-M3/M4/M7 (make check-ports). */
#ifndef BENCH_PORT_ARM_CHECK_H
#define BENCH_PORT_ARM_CHECK_H
#include <stdint.h>
#include "bench_window.h"
static inline uint32_t bench_cycles(void) { return *BENCH_CYCCNT_PTR; }
static inline void bench_putc(char c) { (void)c; }
static inline void bench_puts(const char *s) { (void)s; }
static inline void bench_put_u32(uint32_t v) { (void)v; }
static inline uint32_t bench_clock_hz(void) { return 1u; }
static inline uint32_t bench_target_hz(void) { return 1u; }
#endif
