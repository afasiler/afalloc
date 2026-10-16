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


#endif
