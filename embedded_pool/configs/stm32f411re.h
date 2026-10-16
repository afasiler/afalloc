/* Example pool config for the STM32F411RE (128 KB SRAM): this is the config ../../stm32 uses.
 * !!! PLACEHOLDER numbers (11 KB): size the table from your real workload. */
#ifndef POOL_CONFIG_STM32F411RE_H
#define POOL_CONFIG_STM32F411RE_H

#define POOL_TABLE(X) \
    X(16,  64)        \
    X(32,  64)        \
    X(64,  32)        \
    X(128, 16)        \
    X(256, 16)

/* Put the memory in a linker section so the link fails if it does not fit (the linker script must
 * define it as NOLOAD in RAM: see ../../stm32/linker.ld). Remove this line to use plain .bss. */
#define POOL_SECTION_NAME ".pool_mem"

#endif
