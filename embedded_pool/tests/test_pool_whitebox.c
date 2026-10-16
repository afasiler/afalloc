/* White-box tests: this file #includes pool.c, so it can read and corrupt the allocator's
 * private state directly. It covers what black-box tests cannot reach:
 *   - the pool_check() error codes that need a damaged internal state (4 and 6);
 *   - a bitmap that contradicts the free list (take() must refuse it);
 *   - the boundary between neighbouring pools and the padding between them (the owner map);
 *   - every request size goes to the smallest pool that fits (the size routing table);
 *   - POOL_SPILL accounting (build with -DPOOL_SPILL=1).
 */
#include <stdio.h>
#include <string.h>
#include "../src/pool.c"

#ifdef POOL_EXPECT_BAD_TABLE
#pragma GCC diagnostic ignored "-Wunused-function"   /* only the init check runs in this mode */
#endif

static int failures;

#define EXPECT(cond) do { if (!(cond)) { printf("  FAIL  line %d: %s\n", __LINE__, #cond); failures++; return; } } while (0)

/* Ground truth for "which pool owns this byte", by brute force over the pools. */
static pool_t *owner_by_search(const uint8_t *a)
{
    for (unsigned i = 0; i < NUM_POOLS; i++)
        if (a >= pools[i].base && a < pools[i].base + ((size_t)pools[i].count << pools[i].shift))
            return &pools[i];
    return 0;
}

static void test_owner_map(void)
{
    puts("owner map: every byte of the arena, and the bytes just outside it");
    pool_init();
    for (size_t off = 0; off < ARENA_BYTES; off++)
        EXPECT(find_owner(arena + off) == owner_by_search(arena + off));
    uintptr_t a0 = (uintptr_t)arena;                             /* integers: pointers outside an array are UB */
    EXPECT(find_owner((const void *)(a0 - 1)) == 0);
    EXPECT(find_owner((const void *)(a0 + ARENA_BYTES)) == 0);
    EXPECT(find_owner((const void *)(a0 + ARENA_BYTES + 1000)) == 0);
    EXPECT(find_owner((const void *)1) == 0);
    EXPECT(pool_block_size((const void *)(a0 - 1)) == 0);

    puts("neighbouring pools: the first byte past a pool is the next pool's, or padding");
    for (unsigned i = 0; i + 1 < NUM_POOLS; i++) {
        pool_init();
        uint8_t *end = pools[i].base + ((size_t)pools[i].count << pools[i].shift);
        EXPECT(find_owner(end - 1) == &pools[i]);                /* last byte: this pool */
        uint32_t bad_i = pools[i].bad_free, bad_next = pools[i + 1].bad_free, unowned = misc.unowned_free;
        if (end == pools[i + 1].base) {                          /* really adjacent */
            EXPECT(find_owner(end) == &pools[i + 1]);
            pool_free(end);                                      /* block 0 of the next pool, never handed out */
            EXPECT(pools[i + 1].bad_free == bad_next + 1);
            EXPECT(pools[i].bad_free == bad_i && misc.unowned_free == unowned);
        } else {                                                 /* padding between the two */
            EXPECT(find_owner(end) == 0);
            pool_free(end);                                      /* nobody's block: counted as unowned */
            EXPECT(misc.unowned_free == unowned + 1);
            EXPECT(pools[i].bad_free == bad_i && pools[i + 1].bad_free == bad_next);
        }
    }
}

static void test_routing(void)
{
    puts("routing: every size from 1 to the biggest block goes to the smallest pool that fits");
    for (size_t size = 1; size <= MAX_BLOCK; size++) {
        pool_init();
        unsigned want = 0;
        while (pools[want].block_size < size)
            want++;
        void *b = pool_alloc(size);
        EXPECT(b != 0);
        EXPECT(find_owner(b) == &pools[want]);
        EXPECT(pools[want].in_use == 1);
        pool_free(b);
        EXPECT(pools[want].in_use == 0);
    }
    pool_init();
    EXPECT(pool_alloc(MAX_BLOCK + 1) == 0 && misc.too_big == 1);
    EXPECT(pool_alloc((size_t)-1) == 0 && misc.too_big == 2);
    EXPECT(pool_alloc(0) == 0 && misc.zero_size == 1);
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

/* pool_init() accepts a valid table and rejects one that is not strictly ascending (build with
 * -DPOOL_EXPECT_BAD_TABLE and such a table to check the rejection). */
static void test_init_result(void)
{
#ifdef POOL_EXPECT_BAD_TABLE
    puts("pool_init: a table that is not strictly ascending is rejected");
    EXPECT(pool_init() == -1);
#else
    puts("pool_init: a valid table is accepted");
    EXPECT(pool_init() == 0);
#endif
}

int main(void)
{
    printf("pool white-box tests: %u pools, POOL_DEBUG=%d POOL_SPILL=%d\n", (unsigned)NUM_POOLS, POOL_DEBUG, POOL_SPILL);
    test_init_result();
#ifndef POOL_EXPECT_BAD_TABLE
    test_owner_map();
    test_routing();
    test_check_codes_that_need_internal_state();
#if POOL_SPILL
    test_spill_accounting();
#endif
#endif
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    puts("all white-box tests passed");
    return 0;
}
