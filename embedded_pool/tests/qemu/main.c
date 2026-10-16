/* Runs the library's self-test on an emulated Arm core (QEMU), with no operating system: a minimal
 * vector table, semihosting for output and exit. Built for Cortex-M0, M3, M4 and M7 by
 * `make test-qemu`. It also checks that the pool puts the interrupt mask back the way it found it
 * (PRIMASK), and that this core really has 4-byte pointers. */
#include <stdint.h>
#include "pool.h"
#include "pool_selftest.h"

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;

static void say(const char *s)                      /* semihosting SYS_WRITE0: print a NUL-terminated string */
{
    register uint32_t op __asm("r0") = 4;
    register const char *p __asm("r1") = s;
    __asm volatile("bkpt 0xAB" : "+r"(op) : "r"(p) : "memory");
}

static void say_u32(uint32_t v)
{
    char buf[12];
    int n = 11;
    buf[n] = 0;
    do { buf[--n] = (char)('0' + v % 10u); v /= 10u; } while (v);
    say(&buf[n]);
}

static void finish(int ok)                          /* semihosting SYS_EXIT */
{
    register uint32_t op __asm("r0") = 0x18;
    register uint32_t reason __asm("r1") = ok ? 0x20026u : 0u;
    __asm volatile("bkpt 0xAB" : : "r"(op), "r"(reason) : "memory");
    for (;;) {
    }
}

static void log_line(const char *name, int failed, int line)
{
    say(failed ? "  FAIL  " : "  ok    ");
    say(name);
    if (failed) {
        say(" (pool_selftest.c:");
        say_u32((uint32_t)line);
        say(")");
    }
    say("\n");
}

static inline uint32_t get_primask(void)
{
    uint32_t v;
    __asm volatile("mrs %0, primask" : "=r"(v));
    return v;
}

static inline void set_primask(uint32_t v)
{
    __asm volatile("msr primask, %0" : : "r"(v) : "memory");
}

static int primask_restored(uint32_t initial)
{
    int ok = 1;
    set_primask(initial);
    pool_init();
    void *a = pool_alloc(10);
    ok &= get_primask() == initial;
    pool_free(a);
    ok &= get_primask() == initial;
    void *b = pool_alloc(10);
    ok &= get_primask() == initial;
    pool_free((void *)0);
    pool_alloc(0);
    pool_alloc(100000);
    pool_free((void *)8);
    pool_free((uint8_t *)b + 1);
    ok &= get_primask() == initial;
    pool_free(b);
    ok &= get_primask() == initial;
    void *held[64];
    unsigned n = 0;
    for (;;) {                                       /* exhaust the biggest pool: the slow path */
        void *x = pool_alloc(64);
        ok &= get_primask() == initial;
        if (!x || n == 64)
            break;
        held[n++] = x;
    }
    for (unsigned i = 0; i < n; i++)
        pool_free(held[i]);
    ok &= get_primask() == initial;
    set_primask(0);
    return ok;
}

int main(void)
{
    int failures = 0;
    say("=== library self-test on an emulated core ===\n");
    say("pointer size: ");
    say_u32((uint32_t)sizeof(void *));
    say(" bytes\n");
    failures += sizeof(void *) != 4;

    failures += pool_init() != 0;
    failures += pool_selftest(log_line);

    int m0 = primask_restored(0), m1 = primask_restored(1);
    say(m0 ? "  ok    " : "  FAIL  ");
    say("PRIMASK restored when it was 0\n");
    say(m1 ? "  ok    " : "  FAIL  ");
    say("PRIMASK restored when it was 1\n");
    failures += !m0 + !m1;

    say(failures ? "QEMU TEST FAILED\n" : "QEMU TEST PASSED\n");
    finish(failures == 0);
    return 0;
}

/* ---- start-up: vector table and reset handler ---- */
void Reset_Handler(void);
static void hang(void) { for (;;) { } }

__attribute__((section(".isr_vector"), used))
const void *const vectors[] = { &_estack, Reset_Handler, hang, hang };   /* SP, reset, NMI, HardFault */

void Reset_Handler(void)
{
    const uint32_t *src = &_sidata;
    for (uint32_t *d = &_sdata; d < &_edata;)
        *d++ = *src++;
    for (uint32_t *d = &_sbss; d < &_ebss;)
        *d++ = 0;
    main();
    hang();
}
