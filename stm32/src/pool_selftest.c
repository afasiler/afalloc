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

/* ---- the tests: each returns 0 on success ---- */

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
    REQUIRE(pool_check() == 0);
    return 0;
}

static int t_routing(void)
{
    uint32_t prev = 0;
    for (unsigned i = 0; i < pool_count(); i++) {
        uint32_t s = block_size_of(i);
        void *a = pool_alloc(prev + 1);          /* smallest request that belongs here */
        void *b = pool_alloc(s);                 /* biggest request that belongs here */
        REQUIRE(a != 0 && b != 0);
        REQUIRE(pool_block_size(a) == s && pool_block_size(b) == s);
        REQUIRE(((uintptr_t)a & 7u) == 0 && ((uintptr_t)b & 7u) == 0);
        pool_free(a);
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

static int t_bad_frees(void)
{
    uint8_t outside[32];
    pool_stats_t s0, s1;
    pool_misc_t m0, m1;

    REQUIRE(pool_init() == 0);                    /* fresh state: no block handed out yet */
    pool_free(0);                                 /* ignored silently */

    pool_get_misc(&m0);
    pool_free(outside);                           /* not ours */
    pool_get_misc(&m1);
    REQUIRE(m1.unowned_free == m0.unowned_free + 1);

    uint8_t *p = (uint8_t *)pool_alloc(block_size_of(0));
    REQUIRE(p != 0);
    pool_get_stats(0, &s0);
    pool_free(p + 1);                             /* middle of a block */
    pool_get_stats(0, &s1);
    REQUIRE(s1.bad_free == s0.bad_free + 1);
    REQUIRE(s1.in_use == s0.in_use);              /* nothing was freed */

    /* A block that was never handed out must not be freeable: only block 0 has been used
       since pool_init(), so block 5 is inside the pool but untouched. (Needs this check in
       every configuration, not only with the debug bitmap.) */
    if (count_of(0) >= 6) {
        pool_get_stats(0, &s0);
        pool_free(p + block_size_of(0) * 5);
        pool_get_stats(0, &s1);
        REQUIRE(s1.bad_free == s0.bad_free + 1);
        REQUIRE(s1.in_use == s0.in_use);
    }

#if POOL_DEBUG
    pool_free(p);
    pool_get_stats(0, &s0);
    pool_free(p);                                 /* double free */
    pool_get_stats(0, &s1);
    REQUIRE(s1.bad_free == s0.bad_free + 1);
    REQUIRE(s1.in_use == s0.in_use);
    REQUIRE(pool_check() == 0);
    void *x = pool_alloc(block_size_of(0));       /* the block must be handed out only once */
    void *y = pool_alloc(block_size_of(0));
    REQUIRE(x != 0 && y != 0 && x != y);
    pool_free(x);
    pool_free(y);
#else
    pool_free(p);
#endif
    REQUIRE(pool_check() == 0);
    return 0;
}

static int t_write_after_free(void)
{
#if POOL_DEBUG
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

static int t_peak(void)
{
    pool_stats_reset();
    void *a = pool_alloc(1), *b = pool_alloc(1), *c = pool_alloc(1);
    REQUIRE(a && b && c);
    pool_free(b);
    pool_free(a);
    pool_free(c);
    pool_stats_t s;
    pool_get_stats(0, &s);
    REQUIRE(s.in_use == 0);
    REQUIRE(s.peak == 3);
    return 0;
}

static int t_stress(void)
{
    uint32_t max_size = block_size_of(pool_count() - 1);
    uint32_t live = 0;

    for (uint32_t i = 0; i < TOTAL_BLOCKS; i++)
        slot[i] = 0;

    for (uint32_t step = 0; step < 40000; step++) {
        uint32_t k = rnd() % TOTAL_BLOCKS;
        if (slot[k]) {
            REQUIRE(verify(slot[k], slot_size[k], slot_tag[k]));
            pool_free(slot[k]);
            slot[k] = 0;
            live--;
        } else {
            uint32_t sz = 1 + rnd() % max_size;
            void *p = pool_alloc(sz);
            if (p) {
                REQUIRE(pool_block_size(p) >= sz);
                slot[k] = p;
                slot_size[k] = sz;
                slot_tag[k] = (uint8_t)rnd();
                fill(p, sz, slot_tag[k]);
                live++;
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
    (void)live;
    return 0;
}

typedef struct {
    const char *name;
    int (*fn)(void);
} test_t;

int pool_selftest(selftest_log_t log)
{
    static const test_t tests[] = {
        { "init and configuration",          t_init },
        { "size routing and limits",         t_routing },
        { "exhaust, free and reuse",         t_exhaust_reuse },
        { "LIFO reuse",                      t_lifo },
        { "blocks do not overlap",           t_isolation },
        { "bad and double frees rejected",   t_bad_frees },
        { "write-after-free detected",       t_write_after_free },
        { "peak statistics",                 t_peak },
        { "random stress with data check",   t_stress },
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
