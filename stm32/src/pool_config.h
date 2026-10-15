/* Pool configuration: the ONE place to change the pool sizes.
 *
 * POOL_TABLE(X) lists the pools as X(block_size, block_count), SMALLEST block size
 * first. Rules:
 *   - block_size is a power of two and at least 8 (so every block is 8-byte aligned and
 *     the owner of a pointer can be found with a mask and a shift, no division);
 *   - block_count is at least 1.
 * The memory is a static array per pool, placed in the .pool_mem section of the linker
 * script, so if the pools do not fit in RAM together with the stack reserve the LINK
 * fails ("region RAM overflowed") instead of the program failing at run time.
 *
 * !!! These numbers are PLACEHOLDERS. The right values come from the real workload:
 * for each kind of object, its size and the most of them alive at the same time.
 * pool_get_stats() reports the observed peak for each pool to check the choice.
 */
#ifndef POOL_CONFIG_H
#define POOL_CONFIG_H

#ifndef POOL_TABLE
#define POOL_TABLE(X) \
    X(16,  64)        \
    X(32,  64)        \
    X(64,  32)        \
    X(128, 16)        \
    X(256, 16)
#endif

/* 1 = extra checks: a bitmap per pool to catch double frees, poison bytes on freed blocks
 * to catch writes after free, and pool_check() to verify the free lists. Costs a few cycles
 * and 1 bit per block. Set to 0 for the fastest build. */
#ifndef POOL_DEBUG
#define POOL_DEBUG 1
#endif

/* 1 = when the best-fitting pool is empty, take a block from the next larger pool
 * (still O(number of pools)). 0 = fail instead, which exposes a mis-sized pool at once. */
#ifndef POOL_SPILL
#define POOL_SPILL 0
#endif

#endif
