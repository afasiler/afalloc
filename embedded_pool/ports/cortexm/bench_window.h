/* Tight timing windows for the benchmark on Arm Cortex-M (M3/M4/M7/M33: the DWT cycle counter).
 *
 * Include this from your bench_port.h to define BENCH_TIME_ALLOC / BENCH_TIME_FREE.
 *
 * The counter is read with a volatile load, but the compiler is still free to slide plain
 * instructions (reloading the function pointer from the stack, moving the result, a spill, a
 * compare) between two such loads, and those would be counted as allocator time. So the whole
 * measured sequence is ONE inline-assembly block:
 *        ldr  t0, [counter]      read the cycle counter
 *        blx  fn                 the call being measured
 *        ldr  t1, [counter]      read it again
 * The compiler cannot insert anything inside an asm block. The function pointer and the
 * argument are placed in r3 and r0 before the block; the two timestamps live in callee-saved
 * registers (they must survive the call, and r1, r2, r12 and lr are declared clobbered, which
 * keeps them out of any operand). tools/check_bench_windows.py proves on the compiled object
 * that every call is exactly ldr / blx / ldr. (The asm declares only the caller-saved core
 * registers as clobbered; the harness uses no floating point, so no FP register needs declaring.)
 *
 * BENCH_CYCCNT_ADDR is the address of the 32-bit cycle counter register (default: the Armv7-M
 * DWT_CYCCNT at 0xE0001004). Needs Thumb-2 (not Cortex-M0/M0+, which have no DWT counter anyway).
 */
#ifndef BENCH_WINDOW_CORTEXM_H
#define BENCH_WINDOW_CORTEXM_H

#include <stddef.h>
#include <stdint.h>

#ifndef BENCH_CYCCNT_ADDR
#define BENCH_CYCCNT_ADDR 0xE0001004u
#endif

#define BENCH_CALL_CLOBBERS "r1", "r2", "r12", "lr", "memory", "cc"
#define BENCH_CYCCNT_PTR ((volatile uint32_t *)BENCH_CYCCNT_ADDR)

#define BENCH_TIME_ALLOC(res, fn, arg, t0, t1)                                        \
    do {                                                                              \
        register size_t a0_ __asm("r0") = (arg);                                      \
        register void *(*f3_)(size_t) __asm("r3") = (fn);                             \
        uint32_t s0_, s1_;                                                            \
        __asm volatile("ldr %[s0], [%[cnt]]\n\t"                                      \
                       "blx %[f]\n\t"                                                 \
                       "ldr %[s1], [%[cnt]]"                                          \
                       : [s0] "=&r"(s0_), [s1] "=&r"(s1_), "+r"(a0_), [f] "+r"(f3_)   \
                       : [cnt] "r"(BENCH_CYCCNT_PTR)                                  \
                       : BENCH_CALL_CLOBBERS);                                        \
        (t0) = s0_;                                                                   \
        (t1) = s1_;                                                                   \
        (res) = (void *)a0_;                                                          \
    } while (0)

#define BENCH_TIME_FREE(fn, ptr, t0, t1)                                              \
    do {                                                                              \
        register void *a0_ __asm("r0") = (ptr);                                       \
        register void (*f3_)(void *) __asm("r3") = (fn);                              \
        uint32_t s0_, s1_;                                                            \
        __asm volatile("ldr %[s0], [%[cnt]]\n\t"                                      \
                       "blx %[f]\n\t"                                                 \
                       "ldr %[s1], [%[cnt]]"                                          \
                       : [s0] "=&r"(s0_), [s1] "=&r"(s1_), "+r"(a0_), [f] "+r"(f3_)   \
                       : [cnt] "r"(BENCH_CYCCNT_PTR)                                  \
                       : BENCH_CALL_CLOBBERS);                                        \
        (t0) = s0_;                                                                   \
        (t1) = s1_;                                                                   \
    } while (0)

#endif
