/* Fixed-size block pools.
 *
 * Memory is split ahead of time into several pools; each pool is an array of equally
 * sized blocks. pool_alloc() picks the smallest pool whose blocks fit the request and
 * pops a block off that pool's free list; pool_free() pushes it back. Both take a
 * constant number of steps, there is no search and there is no fragmentation inside a
 * pool, and a block has NO header: its size is implied by the pool it lives in.
 *
 * Blocks are 8-byte aligned. Every public function is safe to call from interrupt
 * handlers as well as from main code (interrupts are masked for a few instructions).
 */
#ifndef POOL_H
#define POOL_H

#include <stddef.h>
#include <stdint.h>

/* Set up the pools. Returns 0, or -1 if the configuration is invalid (sizes not
 * ascending powers of two). Must run once before any other call. Cheap: it does not
 * touch the pool memory (blocks are created lazily on first use). */
int pool_init(void);

/* Allocate at least `size` bytes. Returns NULL if size is 0, larger than the biggest
 * pool, or the pool is exhausted (counted in the statistics). */
void *pool_alloc(size_t size);

/* Return a block. NULL is ignored. Pointers that did not come from pool_alloc(), that
 * point into the middle of a block, or (with POOL_DEBUG) that were already freed are
 * rejected and counted instead of corrupting the pool. */
void pool_free(void *ptr);

/* Size of the block that holds `ptr`, or 0 if the pointer is not inside any pool. */
size_t pool_block_size(const void *ptr);

typedef struct {
    uint32_t block_size;    /* bytes per block */
    uint32_t count;         /* blocks in the pool */
    uint32_t in_use;        /* blocks allocated right now */
    uint32_t peak;          /* highest in_use since init or pool_stats_reset() */
    uint32_t alloc_fail;    /* requests that found this pool empty */
    uint32_t bad_free;      /* rejected frees (middle of a block, never allocated, double free) */
    uint32_t spill_in;      /* blocks lent to smaller requests (POOL_SPILL only) */
} pool_stats_t;

typedef struct {
    uint32_t zero_size;     /* pool_alloc(0) calls */
    uint32_t too_big;       /* requests larger than the biggest pool */
    uint32_t unowned_free;  /* pool_free() of a pointer outside every pool */
} pool_misc_t;

unsigned pool_count(void);
int pool_get_stats(unsigned index, pool_stats_t *out);   /* 0 ok, -1 bad index */
void pool_get_misc(pool_misc_t *out);
void pool_stats_reset(void);                              /* clears counters, keeps blocks */
uint32_t pool_total_bytes(void);                          /* RAM taken by all pool blocks */

/* Walk every free list and verify it (needs POOL_DEBUG for the deep checks): block count
 * adds up, every node is inside its pool and aligned, freed blocks still hold the poison
 * pattern (so nobody wrote to them after free). Returns 0 if healthy, else a nonzero code. */
int pool_check(void);

#endif
