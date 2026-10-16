/* bench_port.h for running the benchmark harness on a computer: a FAKE cycle counter that advances
 * a little on every read, and stdout as the serial port. The numbers it prints mean nothing; it
 * exists so the harness logic can be run under the sanitizers before it is trusted on a board. */
#ifndef BENCH_PORT_HOST_H
#define BENCH_PORT_HOST_H

#include <stdint.h>
#include <stdio.h>

static inline uint32_t bench_fake_step(void)
{
    static uint32_t s = 12345u;
    s = s * 1664525u + 1013904223u;
    return 2u + (s >> 29);                       /* 2..9 fake cycles per counter read */
}

static inline uint32_t bench_cycles(void)
{
    static uint32_t counter;
    counter += bench_fake_step();
    return counter;
}

static inline void bench_putc(char c) { putchar(c); }
static inline void bench_puts(const char *s) { fputs(s, stdout); }
static inline void bench_put_u32(uint32_t v) { printf("%u", (unsigned)v); }
static inline uint32_t bench_clock_hz(void) { return 100000000u; }
static inline uint32_t bench_target_hz(void) { return 100000000u; }

#endif
