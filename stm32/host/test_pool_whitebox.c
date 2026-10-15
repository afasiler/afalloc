/* White-box tests: this file #includes pool.c, so it can read and corrupt the allocator's
 * private state directly. It covers what black-box tests cannot reach:
 *   - the pool_check() error codes that need a damaged internal state (4 and 6);
 *   - a bitmap that contradicts the free list (take() must refuse it);
 *   - the boundary between two ADJACENT pools, as on the board where the pool arrays sit
 *     back to back (on the host the arrays are separate, so the layout is rebuilt here);
 *   - POOL_SPILL accounting (build with -DPOOL_SPILL=1).
 */
#include <stdio.h>
#include <string.h>
#include "../src/pool.c"

static int failures;

#define EXPECT(cond) do { if (!(cond)) { printf("  FAIL  line %d: %s\n", __LINE__, #cond); failures++; return; } } while (0)

/* ---- put every pool back to back in ONE buffer, like the linker does on the board ---- */
#define BUF_BYTES (0 POOL_TABLE(ADD_BYTES))
static uint8_t contiguous[BUF_BYTES] __attribute__((aligned(8)));

static void make_contiguous(void)
{
    unsigned i = 0;
    uint8_t *cur = contiguous;
#if POOL_DEBUG
#define CONTIG(size, count) init_one(&pools[i++], cur, (size), (count), bits_##size); cur += (size_t)(size) * (count);
#else
#define CONTIG(size, count) init_one(&pools[i++], cur, (size), (count), 0); cur += (size_t)(size) * (count);
#endif
    POOL_TABLE(CONTIG)
    misc.zero_size = misc.too_big = misc.unowned_free = misc.corrupt_links = 0;
}

static void test_adjacent_pools(void)
{
    puts("adjacent pools: the first byte past a pool belongs to the next pool");
    make_contiguous();
    for (unsigned i = 0; i + 1 < NUM_POOLS; i++) {
        make_contiguous();
        uint8_t *end = pools[i].base + (size_t)pools[i].count * pools[i].block_size;
        EXPECT(end == pools[i + 1].base);                        /* really adjacent */
        EXPECT(find_owner(end - 1) == &pools[i]);                /* last byte: this pool */
        EXPECT(find_owner(end) == &pools[i + 1]);                /* next byte: the next pool */

        uint32_t bad_i = pools[i].bad_free, bad_next = pools[i + 1].bad_free;
        pool_free(end);                                          /* block 0 of the next pool, never handed out */
        EXPECT(pools[i + 1].bad_free == bad_next + 1);
        EXPECT(pools[i].bad_free == bad_i);
    }
    EXPECT(find_owner(pools[0].base - 1) == 0);
    EXPECT(find_owner(pools[NUM_POOLS - 1].base + (size_t)pools[NUM_POOLS - 1].count * pools[NUM_POOLS - 1].block_size) == 0);
}

static void test_check_codes_that_need_internal_state(void)
{
    puts("pool_check: code 6 (blocks missing) and, with debug, code 4 (bitmap contradicts list)");
    pool_init();
    if (pools[0].count < 2) {
        puts("  (skipped: first pool has fewer than two blocks)");
        return;
    }
    void *a = pool_alloc(pools[0].block_size);
    void *b = pool_alloc(pools[0].block_size);
    EXPECT(a && b);
    pool_free(a);
    pool_free(b);
    EXPECT(pool_check() == 0);

    pools[0].in_use += 1;                                        /* a block "vanished" */
    EXPECT(pool_check() == 6);
    pools[0].in_use -= 1;
    EXPECT(pool_check() == 0);

#if POOL_DEBUG
    uint32_t idx = (uint32_t)(((uint8_t *)b - pools[0].base) >> pools[0].shift);
    pools[0].bitmap[idx >> 5] |= 1u << (idx & 31u);              /* listed free, marked allocated */
    EXPECT(pool_check() == 4);

    /* ...and take() must refuse to hand that block out a second time. */
    uint32_t corrupt0 = misc.corrupt_links;
    void *p = pool_alloc(pools[0].block_size);
    EXPECT(misc.corrupt_links == corrupt0 + 1);
    EXPECT(p != b);                                              /* not the doubly-owned block */
    pools[0].bitmap[idx >> 5] &= ~(1u << (idx & 31u));
#endif
}

#if POOL_SPILL
static void test_spill_accounting(void)
{
    puts("POOL_SPILL: an empty pool borrows from the next larger one, and says so");
    if (NUM_POOLS >= 2) {                                        /* lending needs a larger neighbour */
        pool_init();
        for (uint32_t k = 0; k < pools[0].count; k++)
            EXPECT(pool_alloc(1) != 0);                          /* exhaust the smallest pool */
        EXPECT(pools[0].alloc_fail == 0);

        void *lent = pool_alloc(1);                              /* must come from pool 1 */
        EXPECT(lent != 0);
        EXPECT(pool_block_size(lent) == pools[1].block_size);
        EXPECT(pools[0].alloc_fail == 1);                        /* the best-fit pool was empty */
        EXPECT(pools[0].spill_in == 0);                          /* pool 0 never lends to itself */
        EXPECT(pools[1].spill_in == 1);
        EXPECT(pools[1].alloc_fail == 0);
        EXPECT(pools[1].in_use == 1);
        EXPECT(pool_check() == 0);
    }

    /* A chain: pool 0 and pool 1 are both empty, so a small request is served by pool 2. Only the
       best-fit pool (0) counts a failure; pool 1's failed LENDING attempt must not count as one. */
    if (NUM_POOLS >= 3) {
        pool_init();
        for (uint32_t k = 0; k < pools[0].count; k++)
            EXPECT(pool_alloc(1) != 0);                          /* exhaust pool 0 */
        for (uint32_t k = 0; k < pools[1].count; k++)
            EXPECT(pool_alloc(pools[1].block_size) != 0);        /* exhaust pool 1 (best fit, no lending yet) */
        EXPECT(pools[0].alloc_fail == 0 && pools[1].alloc_fail == 0);

        void *far = pool_alloc(1);
        EXPECT(far != 0);
        EXPECT(pool_block_size(far) == pools[2].block_size);
        EXPECT(pools[0].alloc_fail == 1);                        /* the best-fit pool was empty */
        EXPECT(pools[1].alloc_fail == 0);                        /* a failed lending attempt is not a failure */
        EXPECT(pools[1].spill_in == 0);                          /* ...and it lent nothing */
        EXPECT(pools[2].spill_in == 1);
        EXPECT(pool_check() == 0);
    }

    /* the largest pool has nowhere to spill: one failure, no lending */
    pool_init();
    unsigned last = NUM_POOLS - 1;
    for (uint32_t k = 0; k < pools[last].count; k++)
        EXPECT(pool_alloc(pools[last].block_size) != 0);
    EXPECT(pool_alloc(pools[last].block_size) == 0);
    EXPECT(pools[last].alloc_fail == 1 && pools[last].spill_in == 0);
}
#endif

int main(void)
{
    printf("pool white-box tests: %u pools, POOL_DEBUG=%d POOL_SPILL=%d\n", (unsigned)NUM_POOLS, POOL_DEBUG, POOL_SPILL);
    test_adjacent_pools();
    test_check_codes_that_need_internal_state();
#if POOL_SPILL
    test_spill_accounting();
#endif
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    puts("all white-box tests passed");
    return 0;
}
