/* Test entry used ONLY under QEMU (make emu-test); it replaces main.c.
 *
 * QEMU's STM32F405 machine runs real Cortex-M4 Thumb-2 code, but its clock, GPIO and
 * cycle counter are not the real chip's, so this entry skips them. What it does check,
 * with the real startup code and linker script:
 *   - the vector table: Reset_Handler runs, and interrupts/exceptions taken from several
 *     different slots (SVC, PendSV, SysTick, TIM2 #28, USART2 #38, SPI5 #85, the last entry)
 *     reach exactly the handler of that slot and no other;
 *   - .data was copied from flash IN FULL and .bss was zeroed IN FULL (QEMU's RAM is
 *     pre-filled with 0xA5 by the Makefile and the firmware is loaded as a raw flash image,
 *     because QEMU's ELF loader would zero the .bss segment itself and hide a missing clear);
 *   - the stack pointer is 8-byte aligned in main, and the vector table is the flash alias;
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
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

#define PAINT 0xA5A5A5A5u

/* Core registers used to provoke exceptions (Arm core documentation). */
#define SCB_ICSR   (*(volatile uint32_t *)0xE000ED04u)   /* bit 26 PENDSTSET, bit 28 PENDSVSET */
#define SCB_VTOR   (*(volatile uint32_t *)0xE000ED08u)
#define NVIC_ISER(n) (*(volatile uint32_t *)(0xE000E100u + 4u * (n)))   /* enable IRQ 32n..32n+31 */
#define NVIC_ISPR(n) (*(volatile uint32_t *)(0xE000E200u + 4u * (n)))   /* set pending */

/* Variables whose values prove the startup code did its two jobs. */
static volatile uint32_t data_probe = 0x1234ABCDu;   /* must be copied from flash into RAM */
static volatile uint32_t bss_probe;                  /* must be zero */
static volatile uint8_t bss_block[512];              /* must be all zero */

/* One counter per handler: after each trigger exactly one of them may have moved. */
static volatile uint32_t hits[6];
enum { H_SVC, H_PENDSV, H_SYSTICK, H_TIM2, H_USART2, H_SPI5 };
void SVC_Handler(void)     { hits[H_SVC]++; }
void PendSV_Handler(void)  { hits[H_PENDSV]++; }
void SysTick_Handler(void) { hits[H_SYSTICK]++; }
void TIM2_IRQHandler(void)   { hits[H_TIM2]++; }       /* position 28, slot 44 */
void USART2_IRQHandler(void) { hits[H_USART2]++; }     /* position 38, slot 54 */
void SPI5_IRQHandler(void)   { hits[H_SPI5]++; }       /* position 85, slot 101: the last entry */

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

/* Provoke one exception and require that exactly handler `expect` ran, once. */
static int expect_only(const char *what, int expect, void (*trigger)(void))
{
    uint32_t before[6];
    for (int i = 0; i < 6; i++)
        before[i] = hits[i];
    trigger();
    int ok = 1;
    for (int i = 0; i < 6; i++)
        ok &= hits[i] == before[i] + (i == expect ? 1u : 0u);
    return check(what, ok);
}

static void trig_svc(void)     { __asm volatile("svc 0" : : : "memory"); }
static void trig_pendsv(void)  { SCB_ICSR = 1u << 28; __asm volatile("dsb\n\tisb" : : : "memory"); }
static void trig_systick(void) { SCB_ICSR = 1u << 26; __asm volatile("dsb\n\tisb" : : : "memory"); }
static void trig_tim2(void)    { NVIC_ISER(0) = 1u << 28; NVIC_ISPR(0) = 1u << 28; __asm volatile("dsb\n\tisb" : : : "memory"); }
static void trig_usart2(void)  { NVIC_ISER(1) = 1u << (38 - 32); NVIC_ISPR(1) = 1u << (38 - 32); __asm volatile("dsb\n\tisb" : : : "memory"); }
static void trig_spi5(void)    { NVIC_ISER(2) = 1u << (85 - 64); NVIC_ISPR(2) = 1u << (85 - 64); __asm volatile("dsb\n\tisb" : : : "memory"); }

/* PRIMASK: 1 = interrupts masked. The pool masks them while it works and must put the mask back
 * exactly as it found it, on every path (fast path, slow path, refused free, statistics). */
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

#define MASK_STEP(stmt) do { stmt; ok &= get_primask() == initial; } while (0)

static int primask_restored(uint32_t initial)
{
    int ok = 1;
    set_primask(initial);
    pool_init();
    void *a = pool_alloc(10);                           /* never-used block: slow path */
    ok &= get_primask() == initial;
    MASK_STEP(pool_free(a));
    void *b = pool_alloc(10);                           /* reuse of a freed block: fast path */
    ok &= get_primask() == initial;
    MASK_STEP(pool_free((void *)0));
    MASK_STEP(pool_alloc(0));
    MASK_STEP(pool_alloc(100000));
    MASK_STEP(pool_free((void *)8));                     /* outside every pool */
    MASK_STEP(pool_free((uint8_t *)b + 1));              /* middle of a block */
    MASK_STEP(pool_free(b));
    MASK_STEP(pool_free(b));                             /* double free (refused with POOL_DEBUG) */
    void *held[64];
    unsigned n = 0;
    for (;;) {                                           /* exhaust the 256-byte pool */
        void *x = pool_alloc(256);
        ok &= get_primask() == initial;
        if (!x || n == 64)
            break;
        held[n++] = x;
    }
    for (unsigned i = 0; i < n; i++)
        MASK_STEP(pool_free(held[i]));
    pool_stats_t st;
    pool_misc_t mi;
    MASK_STEP(pool_get_stats(0, &st));
    MASK_STEP(pool_get_misc(&mi));
    MASK_STEP(pool_check());
    MASK_STEP(pool_stats_reset());
    set_primask(0);
    return ok;
}

int main(void)
{
    int failures = 0;

    /* These first: nothing has modified .data/.bss yet. */
    uint32_t sp_in_main;
    __asm volatile("mov %0, sp" : "=r"(sp_in_main));

    int data_ok = 1;
    {
        const uint32_t *flash_copy = &_sidata;
        for (const uint32_t *p = &_sdata; p < &_edata; p++, flash_copy++)
            data_ok &= *p == *flash_copy;                 /* whole range, not one probe word */
    }
    int bss_ok = 1;
    for (const uint32_t *p = &_sbss; p < &_ebss; p++)     /* whole .bss, RAM was pre-filled with 0xA5 */
        bss_ok &= *p == 0;

    paint_stack();
    uart_init(16000000u, 115200u);
    uart_puts("\n=== emulator test (QEMU netduinoplus2, Cortex-M4) ===\n");

    uart_puts("startup code:\n");
    failures += check(".data: every word equals its flash copy", data_ok);
    failures += check(".data probe has its initial value (0x1234ABCD)", data_probe == 0x1234ABCDu);
    failures += check(".bss: every word is zero (RAM was pre-filled with 0xA5)", bss_ok);
    int probe_ok = bss_probe == 0;
    for (unsigned i = 0; i < sizeof bss_block; i++)
        probe_ok &= bss_block[i] == 0;
    failures += check(".bss probes are zero", probe_ok);
    failures += check("stack pointer is 8-byte aligned in main", (sp_in_main & 7u) == 0);
    failures += check("vector table is the flash alias (VTOR 0 or 0x08000000)", SCB_VTOR == 0u || SCB_VTOR == 0x08000000u);

    uart_puts("pointer size on this core: ");
    uart_put_u32((uint32_t)sizeof(void *));
    uart_puts(" bytes\n");

    uart_puts("vector table slots (each trigger must reach exactly its own handler):\n");
    failures += expect_only("SVCall   (slot 11)", H_SVC, trig_svc);
    failures += expect_only("PendSV   (slot 14)", H_PENDSV, trig_pendsv);
    failures += expect_only("SysTick  (slot 15)", H_SYSTICK, trig_systick);
    failures += expect_only("TIM2     (IRQ 28, slot 44)", H_TIM2, trig_tim2);
    failures += expect_only("USART2   (IRQ 38, slot 54)", H_USART2, trig_usart2);
    failures += expect_only("SPI5     (IRQ 85, slot 101, last)", H_SPI5, trig_spi5);

    uart_puts("interrupt masking:\n");
    failures += check("PRIMASK is 0 again after every pool call that started with 0", primask_restored(0));
    failures += check("PRIMASK stays 1 after every pool call that started with 1 (nesting)", primask_restored(1));

    uart_puts("pool self-test:\n");
    failures += pool_init() != 0;
    failures += pool_selftest(log_line);

    uart_puts("stack used: ");
    uart_put_u32(stack_used_bytes());
    uart_puts(" bytes of ");
    uart_put_u32((uint32_t)(uintptr_t)&_Min_Stack_Size);
    uart_puts(" reserved\n");

    uart_puts(failures ? "EMU TEST FAILED\n" : "EMU TEST PASSED\n");
    uart_flush();
    semihost_exit(failures == 0);
    return 0;
}
