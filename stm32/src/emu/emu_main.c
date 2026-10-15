/* Test entry used ONLY under QEMU (make emu-test); it replaces main.c.
 *
 * QEMU's STM32F405 machine runs real Cortex-M4 Thumb-2 code, but its clock, GPIO and
 * cycle counter are not the real chip's, so this entry skips them. What it does check,
 * with the real startup code and linker script:
 *   - the vector table is found and Reset_Handler runs;
 *   - .data was copied from flash and .bss was zeroed;
 *   - the pool self-test passes on a 32-bit ARM core (4-byte pointers, real alignment);
 *   - how much of the stack was actually used (to sanity-check the 4 KB reserve).
 * It then exits QEMU through semihosting with a pass/fail status.
 */
#include <stdint.h>
#include "pool.h"
#include "pool_selftest.h"
#include "uart.h"

extern uint32_t _estack;
extern uint32_t _Min_Stack_Size;      /* absolute symbol: only its address is meaningful */

#define PAINT 0xA5A5A5A5u

/* Variables whose values prove the startup code did its two jobs. */
static volatile uint32_t data_probe = 0x1234ABCDu;   /* must be copied from flash into RAM */
static volatile uint32_t bss_probe;                  /* must be zero */
static volatile uint8_t bss_block[512];              /* must be all zero */

static void semihost_exit(int ok)
{
    register uint32_t op __asm("r0") = 0x18;                      /* SYS_EXIT */
    register uint32_t reason __asm("r1") = ok ? 0x20026u : 0u;    /* ApplicationExit = success */
    __asm volatile("bkpt 0xAB" : : "r"(op), "r"(reason) : "memory");
    for (;;) {
    }
}

static uint32_t *stack_bottom(void)
{
    return (uint32_t *)((uint32_t)&_estack - (uint32_t)&_Min_Stack_Size);
}

static void paint_stack(void)
{
    uint32_t *sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    for (uint32_t *p = stack_bottom(); p < sp - 16; p++)
        *p = PAINT;
}

static uint32_t stack_used_bytes(void)
{
    uint32_t *p = stack_bottom();
    while (p < &_estack && *p == PAINT)
        p++;
    return (uint32_t)((uint8_t *)&_estack - (uint8_t *)p);
}

static void log_line(const char *name, int failed, int line)
{
    uart_puts(failed ? "  FAIL  " : "  ok    ");
    uart_puts(name);
    if (failed) {
        uart_puts(" (pool_selftest.c:");
        uart_put_u32((uint32_t)line);
        uart_puts(")");
    }
    uart_puts("\n");
}

static int check(const char *what, int ok)
{
    uart_puts(ok ? "  ok    " : "  FAIL  ");
    uart_puts(what);
    uart_puts("\n");
    return ok ? 0 : 1;
}

int main(void)
{
    int failures = 0;

    paint_stack();
    uart_init(16000000u, 115200u);
    uart_puts("\n=== emulator test (QEMU netduinoplus2, Cortex-M4) ===\n");

    uart_puts("startup code:\n");
    failures += check(".data copied from flash (0x1234ABCD)", data_probe == 0x1234ABCDu);
    int bss_ok = bss_probe == 0;
    for (unsigned i = 0; i < sizeof bss_block; i++)
        bss_ok &= bss_block[i] == 0;
    failures += check(".bss zeroed", bss_ok);

    uart_puts("pointer size on this core: ");
    uart_put_u32((uint32_t)sizeof(void *));
    uart_puts(" bytes\n");

    uart_puts("pool self-test:\n");
    failures += pool_init() != 0;
    pool_init();
    failures += pool_selftest(log_line);

    uart_puts("stack used: ");
    uart_put_u32(stack_used_bytes());
    uart_puts(" bytes of ");
    uart_put_u32((uint32_t)(uintptr_t)&_Min_Stack_Size);
    uart_puts(" reserved\n");

    uart_puts(failures ? "EMU TEST FAILED\n" : "EMU TEST PASSED\n");
    semihost_exit(failures == 0);
    return 0;
}
