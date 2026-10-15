/* Register map for the parts of the STM32F411 this project uses.
 *
 * Every base address comes from DS10314 Table 10 (p.55-57) / RM0383 Table 1 (p.38-39).
 * Every offset and bit comes from the register description in RM0383; the page is
 * noted next to each block. The _Static_asserts at the bottom check the struct
 * layouts against the offsets in the manual, so a typo fails the build.
 *
 * Core registers (DWT, DEMCR, CPACR) are NOT in RM0383: they are described in the
 * Arm Cortex-M4 documentation. Those addresses are marked "core" and are the first
 * things to confirm on the real board.
 */
#ifndef REGS_H
#define REGS_H

#include <stddef.h>
#include <stdint.h>

typedef volatile uint32_t reg32_t;

/* ---- RCC: reset and clock control. Base 0x4002 3800. RM0383 6.3 (p.103-137) ---- */
typedef struct {
    reg32_t CR;        /* 0x00 */
    reg32_t PLLCFGR;   /* 0x04 */
    reg32_t CFGR;      /* 0x08 */
    reg32_t CIR;       /* 0x0C */
    reg32_t AHB1RSTR;  /* 0x10 */
    reg32_t AHB2RSTR;  /* 0x14 */
    uint32_t _r0[2];   /* 0x18-0x1C */
    reg32_t APB1RSTR;  /* 0x20 */
    reg32_t APB2RSTR;  /* 0x24 */
    uint32_t _r1[2];   /* 0x28-0x2C */
    reg32_t AHB1ENR;   /* 0x30  GPIO/DMA clock enables (p.118) */
    reg32_t AHB2ENR;   /* 0x34 */
    uint32_t _r2[2];   /* 0x38-0x3C */
    reg32_t APB1ENR;   /* 0x40  PWR/USART2/TIM2.. clock enables (p.119) */
    reg32_t APB2ENR;   /* 0x44 */
} RCC_TypeDef;

#define RCC ((RCC_TypeDef *)0x40023800u)

#define RCC_CR_HSION        (1u << 0)
#define RCC_CR_HSIRDY       (1u << 1)
#define RCC_CR_PLLON        (1u << 24)
#define RCC_CR_PLLRDY       (1u << 25)

/* RCC_PLLCFGR (p.105-106): f_VCO = f_in * N / M ; f_sys = f_VCO / P ; f_usb = f_VCO / Q */
#define RCC_PLLCFGR_M_POS   0u      /* [5:0]   2..63, VCO input must be 1..2 MHz */
#define RCC_PLLCFGR_N_POS   6u      /* [14:6]  50..432, VCO output must be 100..432 MHz */
#define RCC_PLLCFGR_P_POS   16u     /* [17:16] 00=/2 01=/4 10=/6 11=/8 */
#define RCC_PLLCFGR_SRC_HSE (1u << 22)  /* 0 = HSI */
#define RCC_PLLCFGR_Q_POS   24u     /* [27:24] 2..15 */
/* The reserved bits must be kept at their reset value, so the code does read-modify-write. */
#define RCC_PLLCFGR_FIELDS  ((0x3Fu << 0) | (0x1FFu << 6) | (0x3u << 16) | (1u << 22) | (0xFu << 24))

/* RCC_CFGR (p.107-109) */
#define RCC_CFGR_SW_MASK    (0x3u << 0)   /* 00 HSI, 01 HSE, 10 PLL */
#define RCC_CFGR_SW_PLL     (0x2u << 0)
#define RCC_CFGR_SWS_MASK   (0x3u << 2)   /* status: same encoding */
#define RCC_CFGR_SWS_PLL    (0x2u << 2)
#define RCC_CFGR_HPRE_MASK  (0xFu << 4)   /* 0xxx = /1 */
#define RCC_CFGR_PPRE1_POS  10u           /* [12:10] 100 = /2 ; APB1 max 50 MHz */
#define RCC_CFGR_PPRE1_MASK (0x7u << 10)
#define RCC_CFGR_PPRE1_DIV2 (0x4u << 10)
#define RCC_CFGR_PPRE2_MASK (0x7u << 13)  /* 0xx = /1 ; APB2 max 100 MHz */

#define RCC_AHB1ENR_GPIOAEN (1u << 0)
#define RCC_APB1ENR_USART2EN (1u << 17)
#define RCC_APB1ENR_PWREN   (1u << 28)

/* ---- PWR. Base 0x4000 7000. RM0383 5.4 (p.86) ---- */
typedef struct {
    reg32_t CR;        /* 0x00 */
    reg32_t CSR;       /* 0x04 */
} PWR_TypeDef;

#define PWR ((PWR_TypeDef *)0x40007000u)
#define PWR_CR_VOS_MASK     (0x3u << 14)
#define PWR_CR_VOS_SCALE1   (0x3u << 14)  /* 11 = scale 1, needed for 100 MHz */

/* ---- Flash interface. Base 0x4002 3C00. RM0383 3.8 (p.59) ---- */
typedef struct {
    reg32_t ACR;       /* 0x00 */
} FLASH_TypeDef;

#define FLASH ((FLASH_TypeDef *)0x40023C00u)
#define FLASH_ACR_LATENCY_MASK (0xFu << 0)  /* wait states; 3 for 90..100 MHz at 2.7..3.6 V (Table 5, p.45) */
#define FLASH_ACR_PRFTEN    (1u << 8)
#define FLASH_ACR_ICEN      (1u << 9)
#define FLASH_ACR_DCEN      (1u << 10)

/* ---- GPIO. GPIOA base 0x4002 0000. RM0383 8.4 (p.158-163) ---- */
typedef struct {
    reg32_t MODER;     /* 0x00  2 bits/pin: 00 in, 01 out, 10 alt function, 11 analog */
    reg32_t OTYPER;    /* 0x04 */
    reg32_t OSPEEDR;   /* 0x08 */
    reg32_t PUPDR;     /* 0x0C */
    reg32_t IDR;       /* 0x10 */
    reg32_t ODR;       /* 0x14 */
    reg32_t BSRR;      /* 0x18  bits 15:0 set pin, bits 31:16 reset pin */
    reg32_t LCKR;      /* 0x1C */
    reg32_t AFR[2];    /* 0x20 AFRL (pins 0-7), 0x24 AFRH (pins 8-15), 4 bits/pin */
} GPIO_TypeDef;

#define GPIOA ((GPIO_TypeDef *)0x40020000u)

/* ---- USART2. Base 0x4000 4400. RM0383 19.6 ---- */
typedef struct {
    reg32_t SR;        /* 0x00 */
    reg32_t DR;        /* 0x04 */
    reg32_t BRR;       /* 0x08 */
    reg32_t CR1;       /* 0x0C */
    reg32_t CR2;       /* 0x10 */
    reg32_t CR3;       /* 0x14 */
} USART_TypeDef;

#define USART2 ((USART_TypeDef *)0x40004400u)
#define USART_SR_RXNE       (1u << 5)
#define USART_SR_TC         (1u << 6)
#define USART_SR_TXE        (1u << 7)
#define USART_CR1_RE        (1u << 2)
#define USART_CR1_TE        (1u << 3)
#define USART_CR1_UE        (1u << 13)

/* ---- Core registers (Arm Cortex-M4, not in RM0383: verify on the board) ---- */
#define CORE_DEMCR          (*(reg32_t *)0xE000EDFCu)   /* bit 24 TRCENA enables the DWT unit */
#define CORE_DEMCR_TRCENA   (1u << 24)
#define CORE_DWT_CTRL       (*(reg32_t *)0xE0001000u)   /* bit 0 CYCCNTENA, bit 25 NOCYCCNT */
#define CORE_DWT_CTRL_CYCCNTENA (1u << 0)
#define CORE_DWT_CTRL_NOCYCCNT  (1u << 25)
#define CORE_DWT_CYCCNT     (*(reg32_t *)0xE0001004u)   /* free-running CPU cycle counter */
#define CORE_CPACR          (*(reg32_t *)0xE000ED88u)   /* coprocessor access: CP10/CP11 = FPU */

/* ---- compile-time checks of the layouts against the manual ---- */
_Static_assert(offsetof(RCC_TypeDef, PLLCFGR)  == 0x04, "RCC_PLLCFGR offset");
_Static_assert(offsetof(RCC_TypeDef, CFGR)     == 0x08, "RCC_CFGR offset");
_Static_assert(offsetof(RCC_TypeDef, AHB1ENR)  == 0x30, "RCC_AHB1ENR offset");
_Static_assert(offsetof(RCC_TypeDef, APB1ENR)  == 0x40, "RCC_APB1ENR offset");
_Static_assert(offsetof(GPIO_TypeDef, ODR)     == 0x14, "GPIO_ODR offset");
_Static_assert(offsetof(GPIO_TypeDef, BSRR)    == 0x18, "GPIO_BSRR offset");
_Static_assert(offsetof(GPIO_TypeDef, AFR)     == 0x20, "GPIO_AFRL offset");
_Static_assert(offsetof(USART_TypeDef, BRR)    == 0x08, "USART_BRR offset");
_Static_assert(offsetof(USART_TypeDef, CR1)    == 0x0C, "USART_CR1 offset");

#endif
