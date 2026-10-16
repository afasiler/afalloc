/* Example pool config for a small MCU with ~8-16 KB of spare RAM (Cortex-M0/M0+ class, e.g.
 * STM32G0/L0/F0, RP2040 with a slice of its 264 KB, nRF51/52). Copy it, edit the table, and build with
 *     -DPOOL_USER_CONFIG='"my_pool_config.h"'
 * Total here: 16*32 + 32*32 + 64*16 + 128*8 = 3584 bytes (the arena adds up to 63 bytes of padding
 * per pool when a pool is not a multiple of POOL_OWNER_GRANULE, and a 1-byte-per-granule owner map).
 *
 * !!! PLACEHOLDER numbers: size the table from your real workload (see README, "Sizing the pools"). */
#ifndef POOL_CONFIG_GENERIC_SMALL_H
#define POOL_CONFIG_GENERIC_SMALL_H

#define POOL_TABLE(X) \
    X(16,  32)        \
    X(32,  32)        \
    X(64,  16)        \
    X(128, 8)

/* Cortex-M0/M0+ have no cycle counter and no count-leading-zeros instruction (the compiler calls
 * a small library routine for it), so alloc is a little slower there than on M3 and up. */

#endif
