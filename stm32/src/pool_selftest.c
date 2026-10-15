#include <stddef.h>
#include <stdint.h>
#include "pool.h"
#include "pool_config.h"
#include "pool_selftest.h"

/* Upper bound on blocks alive at once: the sum of all pool counts. */
#define SUM_COUNT(size, count) + (count)
#define TOTAL_BLOCKS (0 POOL_TABLE(SUM_COUNT))

static void *slot[TOTAL_BLOCKS];
static uint32_t slot_size[TOTAL_BLOCKS];
static uint8_t slot_tag[TOTAL_BLOCKS];

static int g_fail_line;

#define REQUIRE(cond) do { if (!(cond)) { g_fail_line = __LINE__; return 1; } } while (0)

static uint32_t rng_state = 2463534242u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static uint32_t block_size_of(unsigned i)
{
    pool_stats_t s;
    pool_get_stats(i, &s);
    return s.block_size;
}

static uint32_t count_of(unsigned i)
{
    pool_stats_t s;
    pool_get_stats(i, &s);
    return s.count;
}

#if !POOL_SPILL
/* Size of the smallest pool whose blocks fit `size`, computed independently of pool_alloc. */
static uint32_t expected_block(uint32_t size)
{
    for (unsigned i = 0; i < pool_count(); i++)
        if (size <= block_size_of(i))
            return block_size_of(i);
    return 0;
}
#endif

/* Frees the pool refused so far: bad frees in every pool plus frees of foreign pointers. */
static uint32_t rejected_total(void)
{
    pool_misc_t m;
    pool_get_misc(&m);
    uint32_t total = m.unowned_free;
    for (unsigned i = 0; i < pool_count(); i++) {
        pool_stats_t s;
        pool_get_stats(i, &s);
        total += s.bad_free;
    }
    return total;
}

static uint32_t in_use_total(void)
{
    uint32_t total = 0;
    for (unsigned i = 0; i < pool_count(); i++) {
        pool_stats_t s;
        pool_get_stats(i, &s);
        total += s.in_use;
    }
    return total;
}

static void fill(void *p, uint32_t n, uint8_t tag)
{
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++)
        b[i] = (uint8_t)(tag + i);
}

static int verify(const void *p, uint32_t n, uint8_t tag)
{
    const uint8_t *b = (const uint8_t *)p;
    for (uint32_t i = 0; i < n; i++)
        if (b[i] != (uint8_t)(tag + i))
            return 0;
    return 1;
}

/* Read/write the pointer-sized link stored in a free block, the way a buggy caller could.
 * A may_alias view of the raw bytes (no memcpy: the firmware is linked without a C library). */
typedef struct { void *p; } __attribute__((may_alias)) link_t;

static void *get_link(void *block)
{
    return ((link_t *)block)->p;
}

static void set_link(void *block, void *v)
{
    ((link_t *)block)->p = v;
}

/* ---- the tests: each returns 0 on success and starts from a freshly initialised pool ---- */

static int t_init(void)
{
    REQUIRE(pool_init() == 0);
    REQUIRE(pool_count() >= 1);
    for (unsigned i = 0; i < pool_count(); i++) {
        pool_stats_t s;
        REQUIRE(pool_get_stats(i, &s) == 0);
        REQUIRE(s.in_use == 0 && s.peak == 0 && s.alloc_fail == 0 && s.bad_free == 0);
        REQUIRE(s.block_size >= 8 && (s.block_size & (s.block_size - 1)) == 0);
        if (i > 0)
            REQUIRE(s.block_size > block_size_of(i - 1));
    }
    pool_stats_t dummy;
    REQUIRE(pool_get_stats(pool_count(), &dummy) == -1);
    REQUIRE(pool_get_stats(0, 0) == -1);
    REQUIRE(pool_check() == 0);
    return 0;
}

static int t_routing(void)
{
    REQUIRE(pool_init() == 0);
    uint32_t prev = 0;
    for (unsigned i = 0; i < pool_count(); i++) {
        uint32_t s = block_size_of(i);
        void *a = pool_alloc(prev + 1);          /* smallest request that belongs here */
        REQUIRE(a != 0);
        REQUIRE(pool_block_size(a) == s);
        REQUIRE(((uintptr_t)a & 7u) == 0);
        pool_free(a);                            /* one block at a time: a pool may hold just one */
        void *b = pool_alloc(s);                 /* biggest request that belongs here */
        REQUIRE(b != 0);
        REQUIRE(pool_block_size(b) == s);
        REQUIRE(((uintptr_t)b & 7u) == 0);
        pool_free(b);
        prev = s;
    }
    pool_misc_t m0, m1;
    pool_get_misc(&m0);
    REQUIRE(pool_alloc(0) == 0);
    REQUIRE(pool_alloc(prev + 1) == 0);          /* bigger than the biggest pool */
    REQUIRE(pool_alloc((size_t)-1) == 0);
    pool_get_misc(&m1);
    REQUIRE(m1.zero_size == m0.zero_size + 1);
    REQUIRE(m1.too_big == m0.too_big + 2);
    REQUIRE(pool_check() == 0);
    return 0;
}

static int t_exhaust_reuse(void)
{
    REQUIRE(pool_init() == 0);
    for (unsigned i = 0; i < pool_count(); i++) {
        uint32_t size = block_size_of(i), count = count_of(i);
        pool_stats_t s;

        for (uint32_t k = 0; k < count; k++) {
            slot[k] = pool_alloc(size);
            REQUIRE(slot[k] != 0);
            REQUIRE(pool_block_size(slot[k]) == size);
            REQUIRE(((uintptr_t)slot[k] & 7u) == 0);
            for (uint32_t j = 0; j < k; j++)
                REQUIRE(slot[j] != slot[k]);     /* every block distinct */
        }
        pool_get_stats(i, &s);
        REQUIRE(s.in_use == count && s.peak >= count);
#if !POOL_SPILL
        uint32_t fails_before = s.alloc_fail;
        REQUIRE(pool_alloc(size) == 0);           /* pool exhausted */
        pool_get_stats(i, &s);
        REQUIRE(s.alloc_fail == fails_before + 1);
#endif
        for (uint32_t k = 0; k < count; k++)
            pool_free(slot[k]);
        pool_get_stats(i, &s);
        REQUIRE(s.in_use == 0);
        REQUIRE(pool_check() == 0);

        for (uint32_t k = 0; k < count; k++) {   /* all blocks usable again */
            slot[k] = pool_alloc(size);
            REQUIRE(slot[k] != 0);
        }
        for (uint32_t k = 0; k < count; k++)
            pool_free(slot[k]);
    }
    return 0;
}

static int t_lifo(void)
{
    REQUIRE(pool_init() == 0);
    if (count_of(0) < 2)
        return 0;                                 /* needs two blocks in the first pool */
    void *a = pool_alloc(1);
    void *b = pool_alloc(1);
    REQUIRE(a != 0 && b != 0 && a != b);
    pool_free(a);
    REQUIRE(pool_alloc(1) == a);                  /* last freed is first reused */
    pool_free(b);
    pool_free(a);
    REQUIRE(pool_check() == 0);
    return 0;
}

static int t_isolation(void)
{
    /* Neighbouring blocks must not overlap: fill all of one pool with different
       patterns, then verify each one afterwards. */
    REQUIRE(pool_init() == 0);
    for (unsigned i = 0; i < pool_count(); i++) {
        uint32_t size = block_size_of(i), count = count_of(i);
        for (uint32_t k = 0; k < count; k++) {
            slot[k] = pool_alloc(size);
            REQUIRE(slot[k] != 0);
            fill(slot[k], size, (uint8_t)(k * 7 + 1));
        }
        for (uint32_t k = 0; k < count; k++)
            REQUIRE(verify(slot[k], size, (uint8_t)(k * 7 + 1)));
        for (uint32_t k = 0; k < count; k++)
            pool_free(slot[k]);
    }
    return 0;
}

/* Pointers the pool must refuse, tried on EVERY pool: just before its first block, in the middle
 * of a block, and just past its last block (which, on the board, is the first block of the next
 * pool). Nothing else is allocated, so a pointer that lands in a neighbour was never handed out. */
static int t_bad_frees_every_pool(void)
{
    uint8_t outside[32];

    for (unsigned i = 0; i < pool_count(); i++) {
        REQUIRE(pool_init() == 0);
        uint32_t size = block_size_of(i), count = count_of(i);
        uint8_t *first = (uint8_t *)pool_alloc(size);        /* block 0 = the pool's base */
        REQUIRE(first != 0);
        uint32_t before = rejected_total(), live = in_use_total();

        pool_free(first - 1);                                 /* just before the pool */
        REQUIRE(rejected_total() == before + 1 && in_use_total() == live);
        pool_free(first + 1);                                 /* inside block 0 */
        REQUIRE(rejected_total() == before + 2 && in_use_total() == live);
        pool_free(first + (size_t)count * size);              /* one past the last block */
        REQUIRE(rejected_total() == before + 3 && in_use_total() == live);
        uint32_t expect = before + 3;
        if (count >= 2) {
            pool_free(first + (size_t)(count - 1) * size);    /* the last block: never handed out */
            expect++;
            REQUIRE(rejected_total() == expect && in_use_total() == live);
        }
        pool_free(outside);                                   /* not from any pool */
        expect++;
        REQUIRE(rejected_total() == expect && in_use_total() == live);

        pool_free(first);                                     /* the one valid free works */
        REQUIRE(in_use_total() == live - 1 && rejected_total() == expect);
        REQUIRE(pool_check() == 0);
    }
    return 0;
}

static int t_double_free(void)
{
    REQUIRE(pool_init() == 0);
    pool_free(0);                                 /* ignored silently */
    void *p = pool_alloc(block_size_of(0));
    REQUIRE(p != 0);
#if POOL_DEBUG
    pool_free(p);
    uint32_t before = rejected_total();
    pool_free(p);                                 /* double free */
    REQUIRE(rejected_total() == before + 1);
    REQUIRE(pool_check() == 0);
    void *x = pool_alloc(block_size_of(0));       /* the block must be handed out only once */
    void *y = (count_of(0) >= 2) ? pool_alloc(block_size_of(0)) : 0;
    REQUIRE(x != 0);
    REQUIRE(y == 0 || y != x);
    pool_free(x);
    if (y) pool_free(y);
#else
    pool_free(p);                                 /* release builds trust the caller */
#endif
    REQUIRE(pool_check() == 0);
    return 0;
}

/* pool_check() must notice a damaged free list, each kind with its own code. */
static int t_check_detects_damage(void)
{
    REQUIRE(pool_init() == 0);
    if (count_of(0) < 3)
        return 0;
    uint32_t size = block_size_of(0);
    void *a = pool_alloc(size), *b = pool_alloc(size), *c = pool_alloc(size);
    REQUIRE(a && b && c);
    pool_free(a);
    pool_free(b);                                 /* free list: b -> a */
    void *saved = get_link(b);
    REQUIRE(saved == a);

    set_link(b, b);                               /* a cycle */
    REQUIRE(pool_check() == 1);
    uint8_t outside[8];
    set_link(b, outside);                         /* leaves the pool */
    REQUIRE(pool_check() == 2);
    set_link(b, (uint8_t *)a + 1);                /* inside the pool but not at a block start */
    REQUIRE(pool_check() == 3);
    set_link(b, saved);                           /* repaired */
    REQUIRE(pool_check() == 0);
    pool_free(c);
    REQUIRE(pool_check() == 0);
    return 0;
}

static int t_write_after_free(void)
{
#if POOL_DEBUG
    REQUIRE(pool_init() == 0);
    uint32_t size = block_size_of(0);
    if (size <= sizeof(void *))
        return 0;                                 /* no bytes beyond the link to check */
    uint8_t *p = (uint8_t *)pool_alloc(size);
    REQUIRE(p != 0);
    pool_free(p);
    REQUIRE(pool_check() == 0);
    p[size - 1] = 0x11;                           /* a bug in the caller: write after free */
    REQUIRE(pool_check() == 5);                   /* detected */
    p[size - 1] = 0xDD;                           /* repair */
    REQUIRE(pool_check() == 0);
#endif
    return 0;
}

/* A caller that overwrote a freed block's link must not turn an alloc into a wild write.
 * The damaged link is first COPIED when its block is popped (harmless: nothing is written
 * through it); it is found, and abandoned, when the NEXT alloc is about to follow it. */
static int t_corrupt_link_is_not_followed(void)
{
#if POOL_DEBUG
    REQUIRE(pool_init() == 0);
    uint32_t size = block_size_of(0);
    void *a = pool_alloc(size);
    REQUIRE(a != 0);
    pool_free(a);
    uint8_t outside[8];
    set_link(a, outside);                         /* the free list now points outside the pool */

    pool_misc_t m0, m1;
    pool_get_misc(&m0);
    void *first = pool_alloc(size);               /* pops the intact block a */
    REQUIRE(first == a);
    void *second = pool_alloc(size);              /* would follow the bad link: must refuse */
    pool_get_misc(&m1);
    REQUIRE(m1.corrupt_links == m0.corrupt_links + 1);
    REQUIRE(second == 0 || (pool_block_size(second) == size && second != a));   /* never outside the pool */
    REQUIRE(pool_check() == 6 || pool_check() == 0);       /* lost blocks may be reported, nothing else */
#endif
    return 0;
}

static int t_stats_and_reset(void)
{
    REQUIRE(pool_init() == 0);
    uint32_t k = count_of(0) < 3 ? count_of(0) : 3;
    void *held[3] = { 0, 0, 0 };
    for (uint32_t i = 0; i < k; i++) {
        held[i] = pool_alloc(1);
        REQUIRE(held[i] != 0);
    }
    pool_free(held[0]);
    for (uint32_t i = 1; i < k; i++)
        pool_free(held[i]);
    pool_stats_t s;
    pool_get_stats(0, &s);
    REQUIRE(s.in_use == 0 && s.peak == k);

    /* make every resettable counter non-zero, so a reset that forgets one is visible */
    pool_free((void *)&s);                        /* a foreign pointer: unowned_free */
    REQUIRE(pool_alloc(0) == 0);                  /* zero_size */
    REQUIRE(pool_alloc((size_t)-1) == 0);         /* too_big */
    uint32_t cap = count_of(0);
    void *all[TOTAL_BLOCKS];
    uint32_t got = 0;
    for (; got < cap; got++) {                    /* empty pool 0, then fail once: alloc_fail */
        all[got] = pool_alloc(1);
        REQUIRE(all[got] != 0);
    }
#if !POOL_SPILL
    REQUIRE(pool_alloc(1) == 0);
    pool_get_stats(0, &s);
    REQUIRE(s.alloc_fail == 1);
#endif
    pool_free(all[0]);
    pool_free(all[0]);                            /* a second free of the same block: bad_free (debug) */
    pool_misc_t m;
    pool_get_misc(&m);
    REQUIRE(m.unowned_free == 1 && m.zero_size == 1 && m.too_big == 1);
#if POOL_DEBUG
    pool_get_stats(0, &s);
    REQUIRE(s.bad_free >= 1);
#endif
    pool_stats_reset();
    pool_get_misc(&m);
    REQUIRE(m.unowned_free == 0 && m.zero_size == 0 && m.too_big == 0 && m.corrupt_links == 0);
    for (unsigned i = 0; i < pool_count(); i++) {
        pool_get_stats(i, &s);
        REQUIRE(s.peak == s.in_use && s.alloc_fail == 0 && s.bad_free == 0 && s.spill_in == 0);
    }
    for (uint32_t i = 1; i < got; i++)
        pool_free(all[i]);
    return 0;
}

static int t_stress(void)
{
    REQUIRE(pool_init() == 0);
    uint32_t max_size = block_size_of(pool_count() - 1);

    for (uint32_t i = 0; i < TOTAL_BLOCKS; i++)
        slot[i] = 0;

    for (uint32_t step = 0; step < 40000; step++) {
        uint32_t k = rnd() % TOTAL_BLOCKS;
        if (slot[k]) {
            REQUIRE(verify(slot[k], slot_size[k], slot_tag[k]));
            pool_free(slot[k]);
            slot[k] = 0;
        } else {
            /* half the requests are tiny so the small pools are exercised too */
            uint32_t sz = (rnd() & 1) ? 1 + rnd() % 16 : 1 + rnd() % max_size;
            void *p = pool_alloc(sz);
            if (p) {
                REQUIRE(pool_block_size(p) >= sz);
#if !POOL_SPILL
                REQUIRE(pool_block_size(p) == expected_block(sz));   /* the SMALLEST pool that fits */
#endif
                slot[k] = p;
                slot_size[k] = sz;
                slot_tag[k] = (uint8_t)rnd();
                fill(p, sz, slot_tag[k]);
            }
        }
        if ((step & 1023) == 0)
            REQUIRE(pool_check() == 0);
    }
    for (uint32_t k = 0; k < TOTAL_BLOCKS; k++) {
        if (slot[k]) {
            REQUIRE(verify(slot[k], slot_size[k], slot_tag[k]));
            pool_free(slot[k]);
        }
    }
    for (unsigned i = 0; i < pool_count(); i++) {
        pool_stats_t s;
        pool_get_stats(i, &s);
        REQUIRE(s.in_use == 0 && s.peak <= s.count);
    }
    REQUIRE(pool_check() == 0);
    return 0;
}

typedef struct {
    const char *name;
    int (*fn)(void);
} test_t;

int pool_selftest(selftest_log_t log)
{
    static const test_t tests[] = {
        { "init and configuration",                 t_init },
        { "size routing and limits",                t_routing },
        { "exhaust, free and reuse",                t_exhaust_reuse },
        { "LIFO reuse",                             t_lifo },
        { "blocks do not overlap",                  t_isolation },
        { "bad frees refused in every pool",       t_bad_frees_every_pool },
        { "double free",                            t_double_free },
        { "pool_check detects damaged lists",       t_check_detects_damage },
        { "write-after-free detected",              t_write_after_free },
        { "corrupt link is not followed",           t_corrupt_link_is_not_followed },
        { "statistics and reset",                   t_stats_and_reset },
        { "random stress with data check",          t_stress },
    };
    int failures = 0;

    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        g_fail_line = 0;
        int rc = tests[i].fn();
        if (rc)
            failures++;
        log(tests[i].name, rc, g_fail_line);
    }
    return failures;
}
