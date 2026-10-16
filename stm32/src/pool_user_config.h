/* Pool settings for the Nucleo-F411RE (picked up through -DPOOL_USER_CONFIG, see the Makefile).
 * The library's defaults (embedded_pool/include/pool_config.h) cover everything not set here.
 *
 * !!! The table is a PLACEHOLDER (11 KB of 128 KB). The real numbers come from the neural
 * network's small runtime objects: see the README. */
#ifndef POOL_USER_CONFIG_H
#define POOL_USER_CONFIG_H

#define POOL_TABLE(X) \
    X(16,  64)        \
    X(32,  64)        \
    X(64,  32)        \
    X(128, 16)        \
    X(256, 16)

/* Put the pool memory in the linker script's .pool_mem section (NOLOAD, in RAM). */
#define POOL_SECTION_NAME ".pool_mem"

#endif
