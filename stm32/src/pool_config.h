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

/* Upper bound on the memory all pools may take together (a sanity limit checked at compile
 * time; the linker separately checks that everything fits in RAM with the stack). */
#ifndef POOL_MAX_TOTAL_BYTES
#define POOL_MAX_TOTAL_BYTES (96u * 1024u)
#endif

/* 1 = extra checks: a bitmap per pool to catch double frees, poison bytes on freed blocks
 * to catch writes after free, a sanity check of every free-list link before it is followed,
 * and pool_check() to verify the free lists. Costs time (free also touches the whole block)
 * and 1 bit per block. Set to 0 for the fastest, constant-time build. */
#ifndef POOL_DEBUG
#define POOL_DEBUG 1
#endif

/* 1 (default) = alloc/free mask interrupts for their few instructions, so they are safe to
 * call from interrupt handlers. 0 = no masking: faster, but then the allocator must only be
 * used from one context (or the caller must serialise). Lets the benchmark show what the
 * safety costs. */
#ifndef POOL_IRQ_SAFE
#define POOL_IRQ_SAFE 1
#endif

/* 1 = when the best-fitting pool is empty, take a block from the next larger pool
 * (still O(number of pools)). 0 = fail instead, which exposes a mis-sized pool at once. */
#ifndef POOL_SPILL
#define POOL_SPILL 0
#endif

#endif
