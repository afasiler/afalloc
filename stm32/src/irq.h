/* Interrupt-safe critical sections.
 *
 * The pool's alloc and free read-modify-write shared state. If an interrupt
 * handler allocates while the main code is halfway through an alloc, the free
 * list is corrupted. The simplest fix on a single-core Cortex-M is to mask
 * interrupts for those few instructions: irq_save() disables them and returns
 * the previous state, irq_restore() puts it back (so nesting works).
 *
 * PRIMASK is a core register (Arm Cortex-M4 documentation, not RM0383).
 * On the host (POOL_HOST, used for unit tests on the Mac) there are no
 * interrupts, so both functions do nothing.
 */
#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

#ifdef POOL_HOST

static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t state) { (void)state; }

#else

static inline uint32_t irq_save(void)
{
    uint32_t primask;
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask) : : "memory");
    return primask;
}

static inline void irq_restore(uint32_t primask)
{
    __asm volatile("msr primask, %0" : : "r"(primask) : "memory");
}

#endif
#endif
