/* Startup code: the vector table and the reset handler.
 *
 * After reset the CPU reads two words from address 0, which the boot pins alias to
 * flash at 0x0800 0000 (RM0383 2.4, p.41-42):
 *   word 0 = initial stack pointer      word 1 = address of the reset handler
 * Everything after that is the exception/interrupt vector table, in the exact order
 * of RM0383 Table 37 (p.203-205): 16 core entries, then one per interrupt number
 * (position n lives at slot 16 + n, byte address 0x40 + 4n).
 */
#include <stdint.h>
#include "regs.h"

/* Symbols created by linker.ld. Only their ADDRESSES mean anything. */
extern uint32_t _estack;
extern uint32_t _sidata, _sdata, _edata;
extern uint32_t _sbss, _ebss;

int main(void);
void Reset_Handler(void);
void Default_Handler(void);

typedef void (*handler_t)(void);

/* ---- core exceptions: weak, so an application can override them by name ---- */
#define WEAK_DEFAULT __attribute__((weak, alias("Default_Handler")))

void NMI_Handler(void) WEAK_DEFAULT;
void HardFault_Handler(void) WEAK_DEFAULT;
void MemManage_Handler(void) WEAK_DEFAULT;
void BusFault_Handler(void) WEAK_DEFAULT;
void UsageFault_Handler(void) WEAK_DEFAULT;
void SVC_Handler(void) WEAK_DEFAULT;
void DebugMon_Handler(void) WEAK_DEFAULT;
void PendSV_Handler(void) WEAK_DEFAULT;
void SysTick_Handler(void) WEAK_DEFAULT;

/* ---- device interrupts present on the STM32F411 (RM0383 Table 37), by position ----
 * Positions not listed (19-22, 39, 43-46, 48, 52-55, 61-66, 74-80, 82, 83) are reserved. */
#define IRQ_LIST(X) \
    X(0,  WWDG)            X(1,  PVD)             X(2,  TAMP_STAMP)      X(3,  RTC_WKUP) \
    X(4,  FLASH)           X(5,  RCC)             X(6,  EXTI0)           X(7,  EXTI1) \
    X(8,  EXTI2)           X(9,  EXTI3)           X(10, EXTI4)           X(11, DMA1_Stream0) \
    X(12, DMA1_Stream1)    X(13, DMA1_Stream2)    X(14, DMA1_Stream3)    X(15, DMA1_Stream4) \
    X(16, DMA1_Stream5)    X(17, DMA1_Stream6)    X(18, ADC)             X(23, EXTI9_5) \
    X(24, TIM1_BRK_TIM9)   X(25, TIM1_UP_TIM10)   X(26, TIM1_TRG_COM_TIM11) X(27, TIM1_CC) \
    X(28, TIM2)            X(29, TIM3)            X(30, TIM4)            X(31, I2C1_EV) \
    X(32, I2C1_ER)         X(33, I2C2_EV)         X(34, I2C2_ER)         X(35, SPI1) \
    X(36, SPI2)            X(37, USART1)          X(38, USART2)          X(40, EXTI15_10) \
    X(41, RTC_Alarm)       X(42, OTG_FS_WKUP)     X(47, DMA1_Stream7)    X(49, SDIO) \
    X(50, TIM5)            X(51, SPI3)            X(56, DMA2_Stream0)    X(57, DMA2_Stream1) \
    X(58, DMA2_Stream2)    X(59, DMA2_Stream3)    X(60, DMA2_Stream4)    X(67, OTG_FS) \
    X(68, DMA2_Stream5)    X(69, DMA2_Stream6)    X(70, DMA2_Stream7)    X(71, USART6) \
    X(72, I2C3_EV)         X(73, I2C3_ER)         X(81, FPU)             X(84, SPI4) \
    X(85, SPI5)

/* Declare each handler as a weak alias of Default_Handler... */
#define DECLARE_IRQ(pos, name) void name##_IRQHandler(void) WEAK_DEFAULT;
IRQ_LIST(DECLARE_IRQ)

/* ...and put it in the table at slot 16 + position. Unlisted slots stay 0 (reserved). */
#define ENTRY_IRQ(pos, name) [16 + (pos)] = name##_IRQHandler,
#define NUM_VECTORS (16 + 86)

__attribute__((section(".isr_vector"), used))
const handler_t vector_table[NUM_VECTORS] = {
    [0]  = (handler_t)&_estack,        /* initial stack pointer: top of RAM, 0x2002 0000 */
    [1]  = Reset_Handler,
    [2]  = NMI_Handler,
    [3]  = HardFault_Handler,
    [4]  = MemManage_Handler,
    [5]  = BusFault_Handler,
    [6]  = UsageFault_Handler,
    [11] = SVC_Handler,
    [12] = DebugMon_Handler,
    [14] = PendSV_Handler,
    [15] = SysTick_Handler,
    IRQ_LIST(ENTRY_IRQ)
};

/* A fault or an unexpected interrupt lands here. Looping forever keeps the CPU in a
 * known place where a debugger can halt it and look at the registers. */
void Default_Handler(void)
{
    for (;;) {
    }
}

void Reset_Handler(void)
{
    /* The FPU is off after reset. The compiler may use it even for plain data copies
       (hard-float ABI), so switch it on before anything else: CP10 and CP11 full access.
       This file is built with -mgeneral-regs-only (see the Makefile), so no FP/vector
       instruction can be generated before this point. Constructors (__attribute__((constructor)))
       are NOT run: there is no C runtime start-up. */
    CORE_CPACR |= (0xFu << 20);
    __asm volatile("dsb\n\tisb" : : : "memory");

    /* 1. Initialised globals live in flash and run from RAM: copy them over. */
    const uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata;)
        *dst++ = *src++;

    /* 2. Uninitialised globals must start as zero (C requires it). */
    for (uint32_t *dst = &_sbss; dst < &_ebss;)
        *dst++ = 0;

    /* 3. Hand over to the program. */
    main();

    /* main() should never return; if it does, stop here. */
    for (;;) {
    }
}
