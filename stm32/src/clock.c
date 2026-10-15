#include "clock.h"
#include "regs.h"

/* PLL settings (RM0383 6.3.2, p.105-106; limits in DS10314 Table 41, p.89):
 *   VCO input  = 16 MHz / M(8)   = 2 MHz   (allowed 1..2 MHz, 2 recommended; DS 0.95..2.10)
 *   VCO output = 2 MHz  * N      = 2*N MHz (allowed 100..432 MHz)
 *   SYSCLK     = VCO / P(2)                (max 100 MHz)
 *   second output = VCO / Q                (USB needs 48 MHz, SDIO at most 48 MHz, RM p.105)
 */
#define PLL_M 8u
#define PLL_P_BITS 0u              /* 00 = divide by 2 */
#if CLOCK_MHZ == 96
#define PLL_N 96u                  /* VCO 192 MHz -> 96 MHz; Q = 4 gives exactly 48 MHz */
#define PLL_Q 4u
#elif CLOCK_MHZ == 100
#define PLL_N 100u                 /* VCO 200 MHz -> 100 MHz; Q = 5 gives 40 MHz (<= 48) */
#define PLL_Q 5u
#else
#error "CLOCK_MHZ must be 96 or 100 (add the PLL numbers in clock.c for other values)"
#endif

/* The limits from the manuals, checked at compile time so a wrong number is a build error
   and not a chip that silently runs out of spec. These use the NOMINAL 16 MHz HSI. */
#define HSI_HZ 16000000u
#define VCO_IN_HZ  (HSI_HZ / PLL_M)
#define VCO_OUT_HZ (VCO_IN_HZ * PLL_N)
#define SYSCLK_HZ  (VCO_OUT_HZ / (2u * (PLL_P_BITS + 1u)))
#define PLL_Q_HZ   (VCO_OUT_HZ / PLL_Q)
_Static_assert(PLL_M >= 2 && PLL_M <= 63, "PLLM must be 2..63");
_Static_assert(VCO_IN_HZ >= 1000000u && VCO_IN_HZ <= 2000000u, "VCO input must be 1..2 MHz");
_Static_assert(PLL_N >= 50 && PLL_N <= 432, "PLLN must be 50..432");
_Static_assert(VCO_OUT_HZ >= 100000000u && VCO_OUT_HZ <= 432000000u, "VCO output must be 100..432 MHz");
_Static_assert(PLL_P_BITS <= 3, "PLLP field is 2 bits");
_Static_assert(PLL_Q >= 2 && PLL_Q <= 15, "PLLQ must be 2..15");
_Static_assert(PLL_Q_HZ <= 48000000u, "PLL second output (USB/SDIO) must not exceed 48 MHz");
_Static_assert(SYSCLK_HZ == CLOCK_MHZ * 1000000u, "PLL numbers must produce CLOCK_MHZ");
_Static_assert(SYSCLK_HZ <= 100000000u, "system clock must not exceed 100 MHz");
_Static_assert(SYSCLK_HZ / 2u <= 50000000u, "APB1 (HCLK/2) must not exceed 50 MHz");

/* 3 wait states for 90 < HCLK <= 100 MHz at 2.7-3.6 V (RM0383 Table 5, p.45). The board
 * measured 3.24 V, inside that range. Valid for 96 MHz as well. */
#define FLASH_LATENCY 3u
_Static_assert(SYSCLK_HZ > 90000000u && SYSCLK_HZ <= 100000000u, "flash latency below is for 90..100 MHz");

#define WAIT_LOOPS 200000u

static uint32_t g_hclk = HSI_HZ;
static uint32_t g_pclk1 = HSI_HZ;

static int wait_for(volatile uint32_t *reg, uint32_t mask, uint32_t value)
{
    for (uint32_t i = 0; i < WAIT_LOOPS; i++)
        if ((*reg & mask) == value)
            return 0;
    return -1;
}

/* Back to the known reset state: HSI as system clock, PLL off, APB1 undivided. */
static int return_to_hsi(void)
{
    RCC->CR |= RCC_CR_HSION;
    if (wait_for(&RCC->CR, RCC_CR_HSIRDY, RCC_CR_HSIRDY) != 0)
        return -1;
    RCC->CFGR &= ~RCC_CFGR_SW_MASK;                           /* SW = 00: HSI */
    if (wait_for(&RCC->CFGR, RCC_CFGR_SWS_MASK, 0) != 0)
        return -1;
    RCC->CR &= ~RCC_CR_PLLON;
    if (wait_for(&RCC->CR, RCC_CR_PLLRDY, 0) != 0)
        return -1;
    RCC->CFGR &= ~(RCC_CFGR_HPRE_MASK | RCC_CFGR_PPRE1_MASK | RCC_CFGR_PPRE2_MASK);
    g_hclk = HSI_HZ;
    g_pclk1 = HSI_HZ;
    return 0;
}

static int fail(int stage)
{
    return return_to_hsi() == 0 ? stage : 5;
}

int clock_init_pll(void)
{
    /* The PLL fields below can only be written while the PLL is off, and PLLON cannot be
       cleared while the PLL is the system clock (RM0383 p.104-106). So start from a known
       state even if something (a bootloader, an earlier call) already switched. */
    if (return_to_hsi() != 0)
        return 5;

    /* Voltage scale 1 is required above 84 MHz (RM0383 5.4.1, p.86). The PWR peripheral has
       to be clocked before its register can be written; VOS can only change while the PLL is off. */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;                       /* read back: let the clock enable settle */
    PWR->CR = (PWR->CR & ~PWR_CR_VOS_MASK) | PWR_CR_VOS_SCALE1;

    /* Increasing the frequency: raise the flash wait states FIRST, confirm, then switch the
       clock (RM0383 p.45). Also switch on the ART accelerator (prefetch + caches). */
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_LATENCY;
    if (wait_for(&FLASH->ACR, FLASH_ACR_LATENCY_MASK, FLASH_LATENCY) != 0)
        return fail(1);

    /* Read-modify-write so the reserved bits keep their reset value. Source bit left at 0 = HSI. */
    RCC->PLLCFGR = (RCC->PLLCFGR & ~RCC_PLLCFGR_FIELDS)
                 | (PLL_M << RCC_PLLCFGR_M_POS)
                 | (PLL_N << RCC_PLLCFGR_N_POS)
                 | (PLL_P_BITS << RCC_PLLCFGR_P_POS)
                 | (PLL_Q << RCC_PLLCFGR_Q_POS);

    RCC->CR |= RCC_CR_PLLON;
    if (wait_for(&RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY) != 0)
        return fail(2);

    /* The regulator only moves to scale 1 once the PLL runs; wait until it says so before
       raising the clock that depends on it (PWR_CSR.VOSRDY, RM0383 p.88). */
    if (wait_for(&PWR->CSR, PWR_CSR_VOSRDY, PWR_CSR_VOSRDY) != 0)
        return fail(4);

    /* Bus dividers BEFORE the switch, so APB1 never exceeds its 50 MHz limit:
       AHB /1, APB1 /2, APB2 /1. */
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE_MASK | RCC_CFGR_PPRE1_MASK | RCC_CFGR_PPRE2_MASK))
              | RCC_CFGR_PPRE1_DIV2;

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_MASK) | RCC_CFGR_SW_PLL;
    if (wait_for(&RCC->CFGR, RCC_CFGR_SWS_MASK, RCC_CFGR_SWS_PLL) != 0)
        return fail(3);

    g_hclk = SYSCLK_HZ;
    g_pclk1 = SYSCLK_HZ / 2u;
    return 0;
}

uint32_t clock_hclk_hz(void)   { return g_hclk; }
uint32_t clock_pclk1_hz(void)  { return g_pclk1; }
uint32_t clock_target_hz(void) { return SYSCLK_HZ; }
