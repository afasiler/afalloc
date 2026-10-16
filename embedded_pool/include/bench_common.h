/* Cycle-accurate benchmark of any allocator with the malloc/free signature. The same harness times
 * the pool and a C library's malloc, so the numbers are directly comparable. Each call goes through
 * a function pointer in both cases.
 *
 * It needs a CPU cycle counter and a way to print. Both come from "bench_port.h", a header YOUR
 * project provides (include path), so the harness itself is hardware independent. It must define:
 *     uint32_t bench_cycles(void);        free-running 32-bit counter, one tick per CPU cycle
 *                                         (unsigned subtraction handles one wrap)
 *     void     bench_putc(char c);
 *     void     bench_puts(const char *s);
 *     void     bench_put_u32(uint32_t v); decimal
 *     uint32_t bench_clock_hz(void);      the CPU clock actually running (for ns figures)
 *     uint32_t bench_target_hz(void);     the clock it was meant to run at (a mismatch is flagged
 *                                         in the output); return bench_clock_hz() if not applicable
 * and may define:
 *     BENCH_TIME_ALLOC(res, fn, arg, t0, t1), BENCH_TIME_FREE(fn, ptr, t0, t1)
 *                                         tighter timing windows (ports/cortexm/bench_window.h)
 *     BENCH_CACHE_FLUSH()                 empty the code cache for the "cold" scenario
 * Examples: ../stm32/src/bench_port.h (Cortex-M4 DWT) and tests/bench_port.h (a computer). */
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <stddef.h>

typedef void *(*bench_alloc_fn)(size_t);
typedef void (*bench_free_fn)(void *);

/* Runs the scenarios and prints the results on the serial port. */
void bench_run(const char *name, bench_alloc_fn alloc, bench_free_fn release);

#endif
