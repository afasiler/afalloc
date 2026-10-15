/* Interrupt-safe critical sections.
 *
 * The pool's alloc and free read-modify-write shared state. If an interrupt handler
 * allocates while the main code is halfway through an alloc, the free list is corrupted.
 * The simplest fix on a single-core Cortex-M is to mask interrupts for those few
 * instructions: irq_save() disables them and returns the previous state, irq_restore()
 * puts it back (so nesting works).
 *
 * PRIMASK is a core register (Arm Cortex-M4 documentation, not RM0383). It masks every
 * configurable interrupt but NOT NMI or HardFault, so the allocator must not be called
 * from those two handlers.
 *
 * On the host (POOL_HOST, used for unit tests on the Mac) there are no interrupts, and with
 * POOL_IRQ_SAFE=0 the caller promises a single context: in both cases the functions do
 * nothing.
 */
#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>
#include "pool_config.h"

#if defined(POOL_HOST) || !POOL_IRQ_SAFE

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
