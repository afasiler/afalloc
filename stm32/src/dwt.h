/* CPU cycle counter (DWT CYCCNT): counts one per CPU clock cycle, 32 bits wide,
 * so it wraps after 2^32 cycles (about 43 s at 100 MHz). Unsigned subtraction
 * of two readings is correct across one wrap, so durations below ~43 s are safe.
 *
 * Enabling sequence (Arm core documentation): set TRCENA in DEMCR to switch on
 * the debug/trace block, clear CYCCNT, set CYCCNTENA in DWT_CTRL.
 * The counter is optional in the core: NOCYCCNT in DWT_CTRL says if it is missing.
 */
#ifndef DWT_H
#define DWT_H

#include <stdint.h>
#include "regs.h"

#ifdef POOL_HOST
/* On the Mac there is no cycle counter: a fake one that advances a little on every read,
   so the benchmark harness can be run and checked for logic errors (the numbers it prints
   there mean nothing). */
extern uint32_t host_cycle_counter;
extern uint32_t host_cycle_step(void);
static inline int dwt_init(void) { return 0; }
static inline uint32_t cycles(void) { host_cycle_counter += host_cycle_step(); return host_cycle_counter; }
#else

/* Returns 0 on success, -1 if this core has no cycle counter. */
static inline int dwt_init(void)
{
    CORE_DEMCR |= CORE_DEMCR_TRCENA;
    if (CORE_DWT_CTRL & CORE_DWT_CTRL_NOCYCCNT)
        return -1;
    CORE_DWT_CYCCNT = 0;
    CORE_DWT_CTRL |= CORE_DWT_CTRL_CYCCNTENA;
    return 0;
}

static inline uint32_t cycles(void)
{
    return CORE_DWT_CYCCNT;
}

#endif /* POOL_HOST */

#endif
