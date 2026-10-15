#include "clock.h"
#include "regs.h"

/* PLL settings for 100 MHz from the 16 MHz HSI (RM0383 6.3.2, p.105-106):
 *   VCO input  = 16 MHz / M(8)   = 2 MHz   (allowed 1..2 MHz, 2 recommended)
 *   VCO output = 2 MHz  * N(100) = 200 MHz (allowed 100..432 MHz)
 *   SYSCLK     = 200 MHz / P(2)  = 100 MHz (max 100 MHz)
 *   Q only matters for USB/SDIO, which we do not use; 4 is the reset value.
 */
#define PLL_M 8u
#define PLL_N 100u
#define PLL_P_BITS 0u      /* 00 = divide by 2 */
#define PLL_Q 4u

/* The limits from RM0383 p.105-106 and DS10314 p.20-21, checked at compile time so a wrong
   number is a build error and not a chip that silently runs out of spec. */
#define HSI_HZ 16000000u
#define VCO_IN_HZ  (HSI_HZ / PLL_M)
#define VCO_OUT_HZ (VCO_IN_HZ * PLL_N)
#define SYSCLK_HZ  (VCO_OUT_HZ / (2u * (PLL_P_BITS + 1u)))
_Static_assert(PLL_M >= 2 && PLL_M <= 63, "PLLM must be 2..63");
_Static_assert(VCO_IN_HZ >= 1000000u && VCO_IN_HZ <= 2000000u, "VCO input must be 1..2 MHz");
_Static_assert(PLL_N >= 50 && PLL_N <= 432, "PLLN must be 50..432");
_Static_assert(VCO_OUT_HZ >= 100000000u && VCO_OUT_HZ <= 432000000u, "VCO output must be 100..432 MHz");
_Static_assert(PLL_P_BITS <= 3, "PLLP field is 2 bits");
_Static_assert(PLL_Q >= 2 && PLL_Q <= 15, "PLLQ must be 2..15");
_Static_assert(SYSCLK_HZ == 100000000u, "system clock must come out at exactly 100 MHz");
_Static_assert(SYSCLK_HZ / 2u <= 50000000u, "APB1 (HCLK/2) must not exceed 50 MHz");

/* 3 wait states for 90 < HCLK <= 100 MHz at 2.7-3.6 V (RM0383 Table 5, p.45). The
 * board measured 3.24 V, which is inside that range. */
#define FLASH_LATENCY_100MHZ 3u

#define WAIT_LOOPS 200000u

static uint32_t g_hclk = 16000000u;
static uint32_t g_pclk1 = 16000000u;

static int wait_until_set(volatile uint32_t *reg, uint32_t mask)
{
    for (uint32_t i = 0; i < WAIT_LOOPS; i++)
        if ((*reg & mask) == mask)
            return 0;
    return -1;
}

int clock_init_100mhz(void)
{
    /* Voltage scale 1 is required above 84 MHz (RM0383 5.4.1, p.86). The PWR peripheral
       has to be clocked before its register can be written. */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;                       /* read back: let the clock enable settle */
    PWR->CR = (PWR->CR & ~PWR_CR_VOS_MASK) | PWR_CR_VOS_SCALE1;

    /* Increasing the frequency: raise the flash wait states FIRST, confirm, then switch
       the clock (RM0383 p.45). Also switch on the ART accelerator (prefetch + caches). */
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_LATENCY_100MHZ;
    {
        int ok = 0;
        for (uint32_t i = 0; i < WAIT_LOOPS; i++) {
            if ((FLASH->ACR & FLASH_ACR_LATENCY_MASK) == FLASH_LATENCY_100MHZ) { ok = 1; break; }
        }
        if (!ok)
            return 1;
    }

    /* PLL parameters can only be written while the PLL is off. Read-modify-write so the
       reserved bits keep their reset value. Source bit left at 0 = HSI. */
    RCC->PLLCFGR = (RCC->PLLCFGR & ~RCC_PLLCFGR_FIELDS)
                 | (PLL_M << RCC_PLLCFGR_M_POS)
                 | (PLL_N << RCC_PLLCFGR_N_POS)
                 | (PLL_P_BITS << RCC_PLLCFGR_P_POS)
                 | (PLL_Q << RCC_PLLCFGR_Q_POS);

    RCC->CR |= RCC_CR_PLLON;
    if (wait_until_set(&RCC->CR, RCC_CR_PLLRDY) != 0)
        return 2;

    /* Bus dividers BEFORE the switch, so APB1 never exceeds its 50 MHz limit:
       AHB /1, APB1 /2, APB2 /1. */
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE_MASK | RCC_CFGR_PPRE1_MASK | RCC_CFGR_PPRE2_MASK))
              | RCC_CFGR_PPRE1_DIV2;

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_MASK) | RCC_CFGR_SW_PLL;
    if (wait_until_set(&RCC->CFGR, RCC_CFGR_SWS_PLL) != 0)
        return 3;

    g_hclk = 100000000u;
    g_pclk1 = 50000000u;
    return 0;
}

uint32_t clock_hclk_hz(void)  { return g_hclk; }
uint32_t clock_pclk1_hz(void) { return g_pclk1; }
