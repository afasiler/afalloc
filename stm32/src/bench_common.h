/* Cycle-accurate benchmark of any allocator with the malloc/free signature, using the
 * DWT cycle counter. The same harness times the pool and newlib's malloc, so the numbers
 * are directly comparable. Each call goes through a function pointer in both cases. */
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <stddef.h>

typedef void *(*bench_alloc_fn)(size_t);
typedef void (*bench_free_fn)(void *);

/* Runs the scenarios and prints the results on the serial port. */
void bench_run(const char *name, bench_alloc_fn alloc, bench_free_fn release);

#endif
