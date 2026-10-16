/* Port layer: the ONLY hardware-dependent part of the pool allocator.
 *
 * The pool's alloc and free read-modify-write shared state. If an interrupt handler allocates
 * while the main code is halfway through an alloc, the free list is corrupted. The simplest fix on
 * a single-core MCU is to mask interrupts for those few instructions: pool_irq_save() disables them
 * and returns the previous state, pool_irq_restore() puts it back (so nesting works).
 *
 * Which implementation is used, in this order:
 *   1. POOL_HOST defined (unit tests on a computer), or POOL_IRQ_SAFE=0: nothing is masked. With
 *      POOL_IRQ_SAFE=0 the caller promises that the pool is used from one context only, or
 *      serialises access itself.
 *   2. POOL_PORT_HEADER defined (e.g. -DPOOL_PORT_HEADER='"my_port.h"'): your header must define
 *      `static inline uint32_t pool_irq_save(void)` and `static inline void pool_irq_restore(uint32_t)`.
 *   3. An Arm Cortex-M core (detected from the compiler): PRIMASK, below. This covers Cortex-M0,
 *      M0+, M3, M4, M7, M23, M33 alike. PRIMASK masks every configurable interrupt but NOT NMI or
 *      HardFault, so the pool must not be called from those two handlers.
 *   4. Anything else: a compile error telling you to pick 1 or 2. (A RISC-V or Xtensa port is a
 *      few lines of assembly in a POOL_PORT_HEADER, but is not shipped because it has not been run.)
 */
#ifndef POOL_PORT_H
#define POOL_PORT_H

#include <stdint.h>
#include "pool_config.h"

#if defined(POOL_HOST) || !POOL_IRQ_SAFE

static inline uint32_t pool_irq_save(void) { return 0; }
static inline void pool_irq_restore(uint32_t state) { (void)state; }

#elif defined(POOL_PORT_HEADER)

#include POOL_PORT_HEADER

#elif defined(__arm__) && defined(__ARM_ARCH_PROFILE) && __ARM_ARCH_PROFILE == 'M'

static inline uint32_t pool_irq_save(void)
{
    uint32_t primask;
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask) : : "memory");
    return primask;
}

static inline void pool_irq_restore(uint32_t primask)
{
    __asm volatile("msr primask, %0" : : "r"(primask) : "memory");
}

#else
#error "pool_port.h: no interrupt-masking port for this CPU. Build with -DPOOL_IRQ_SAFE=0 (single context), or provide -DPOOL_PORT_HEADER='\"your_port.h\"' defining pool_irq_save()/pool_irq_restore()."
#endif

#endif
