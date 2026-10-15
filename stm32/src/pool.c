#include "pool.h"
#include "pool_config.h"
#include "irq.h"

#ifdef POOL_HOST
#define POOL_SECTION            /* host tests: ordinary static arrays */
#else
#define POOL_SECTION __attribute__((section(".pool_mem")))
#endif

#define POISON_BYTE 0xDDu

/* A free block stores the link to the next free block in its own first bytes: the block
 * is not in use, so that memory is free to hold bookkeeping. may_alias tells the compiler
 * this pointer-sized view of raw bytes is intentional. */
typedef struct free_node {
    struct free_node *next;
} __attribute__((may_alias)) free_node_t;

typedef struct {
    uint8_t *base;              /* first block */
    uint32_t block_size;        /* power of two */
    uint32_t shift;             /* log2(block_size): index = offset >> shift */
    uint32_t count;             /* number of blocks */
    free_node_t *free_head;     /* top of the free list (LIFO) */
    uint32_t fresh;             /* blocks [0, fresh) have been handed out at least once */
    uint32_t in_use, peak, alloc_fail, bad_free, spill_in;
#if POOL_DEBUG
    uint32_t *bitmap;           /* 1 bit per block, 1 = currently allocated */
#endif
} pool_t;

/* ---- static storage generated from POOL_TABLE ---- */

/* One byte array per pool, 8-byte aligned, in .pool_mem. */
#define DEFINE_MEM(size, count) \
    static uint8_t mem_##size[(size) * (count)] POOL_SECTION __attribute__((aligned(8)));
POOL_TABLE(DEFINE_MEM)

#if POOL_DEBUG
#define DEFINE_BITS(size, count) static uint32_t bits_##size[((count) + 31) / 32];
POOL_TABLE(DEFINE_BITS)
#endif

/* Compile-time checks on the table. */
#define CHECK_ENTRY(size, count) \
    _Static_assert((size) >= 8 && ((size) & ((size) - 1)) == 0, "pool block size must be a power of two >= 8"); \
    _Static_assert((count) >= 1, "pool needs at least one block");
POOL_TABLE(CHECK_ENTRY)

#define PLUS_ONE(size, count) + 1
#define NUM_POOLS (0 POOL_TABLE(PLUS_ONE))

static pool_t pools[NUM_POOLS];
static pool_misc_t misc;

/* ---- small helpers ---- */

#if POOL_DEBUG
static void poison(uint8_t *p, uint32_t n)
{
    while (n--)
        *p++ = POISON_BYTE;
}
#endif

static void init_one(pool_t *p, uint8_t *base, uint32_t size, uint32_t count, uint32_t *bitmap)
{
    p->base = base;
    p->block_size = size;
    p->shift = 0;
    while ((1u << p->shift) < size)
        p->shift++;
    p->count = count;
    p->free_head = 0;
    p->fresh = 0;
    p->in_use = p->peak = p->alloc_fail = p->bad_free = p->spill_in = 0;
#if POOL_DEBUG
    p->bitmap = bitmap;
    for (uint32_t i = 0; i < (count + 31) / 32; i++)
        bitmap[i] = 0;
#else
    (void)bitmap;
#endif
}

int pool_init(void)
{
    unsigned i = 0;
#if POOL_DEBUG
#define INIT_ENTRY(size, count) init_one(&pools[i++], mem_##size, (size), (count), bits_##size);
#else
#define INIT_ENTRY(size, count) init_one(&pools[i++], mem_##size, (size), (count), 0);
#endif
    POOL_TABLE(INIT_ENTRY)

    misc.zero_size = misc.too_big = misc.unowned_free = 0;

    for (unsigned k = 1; k < NUM_POOLS; k++)
        if (pools[k].block_size <= pools[k - 1].block_size)
            return -1;                                  /* must be strictly ascending */
    return 0;
}

/* Pop one block from `p`, or NULL. Caller holds the critical section. */
static void *take(pool_t *p, int lent)
{
    free_node_t *n = p->free_head;

    if (n) {
        p->free_head = n->next;                         /* reuse a freed block */
    } else if (p->fresh < p->count) {
        /* Every block is at least 8-byte aligned (base is aligned(8), sizes are powers of two >= 8),
           so this cast is safe; going through void * tells the compiler we know. */
        n = (free_node_t *)(void *)(p->base + (size_t)p->fresh * p->block_size);   /* first use */
        p->fresh++;
    } else {
        if (!lent)
            p->alloc_fail++;
        return 0;
    }

    p->in_use++;
    if (p->in_use > p->peak)
        p->peak = p->in_use;
    if (lent)
        p->spill_in++;
#if POOL_DEBUG
    {
        uint32_t idx = (uint32_t)(((uint8_t *)n - p->base) >> p->shift);
        p->bitmap[idx >> 5] |= 1u << (idx & 31u);
    }
#endif
    return n;
}

void *pool_alloc(size_t size)
{
    void *result = 0;
    uint32_t state = irq_save();

    if (size == 0) {
        misc.zero_size++;
    } else {
        unsigned i = 0;
        while (i < NUM_POOLS && size > pools[i].block_size)
            i++;                                        /* smallest pool that fits */
        if (i == NUM_POOLS) {
            misc.too_big++;
        } else {
            result = take(&pools[i], 0);
#if POOL_SPILL
            for (unsigned j = i + 1; result == 0 && j < NUM_POOLS; j++)
                result = take(&pools[j], 1);
#endif
        }
    }

    irq_restore(state);
    return result;
}

static pool_t *find_owner(const void *ptr)
{
    uintptr_t a = (uintptr_t)ptr;
    for (unsigned i = 0; i < NUM_POOLS; i++) {
        uintptr_t lo = (uintptr_t)pools[i].base;
        if (a >= lo && a < lo + (uintptr_t)pools[i].count * pools[i].block_size)
            return &pools[i];
    }
    return 0;
}

void pool_free(void *ptr)
{
    if (ptr == 0)
        return;

    uint32_t state = irq_save();
    pool_t *p = find_owner(ptr);

    if (p == 0) {
        misc.unowned_free++;
    } else {
        uint32_t off = (uint32_t)((uint8_t *)ptr - p->base);
        uint32_t idx = off >> p->shift;
        int ok = (off & (p->block_size - 1u)) == 0u      /* start of a block */
              && idx < p->fresh;                         /* ...that was handed out before */
#if POOL_DEBUG
        if (ok) {
            uint32_t bit = 1u << (idx & 31u);
            if ((p->bitmap[idx >> 5] & bit) == 0)        /* already free: double free */
                ok = 0;
            else
                p->bitmap[idx >> 5] &= ~bit;
        }
        if (ok)
            poison((uint8_t *)ptr, p->block_size);
#endif
        if (!ok) {
            p->bad_free++;
        } else {
            free_node_t *n = (free_node_t *)ptr;
            n->next = p->free_head;
            p->free_head = n;
            p->in_use--;
        }
    }

    irq_restore(state);
}

size_t pool_block_size(const void *ptr)
{
    pool_t *p = find_owner(ptr);
    return p ? p->block_size : 0;
}

unsigned pool_count(void)
{
    return NUM_POOLS;
}

int pool_get_stats(unsigned index, pool_stats_t *out)
{
    if (index >= NUM_POOLS)
        return -1;
    uint32_t state = irq_save();
    const pool_t *p = &pools[index];
    out->block_size = p->block_size;
    out->count = p->count;
    out->in_use = p->in_use;
    out->peak = p->peak;
    out->alloc_fail = p->alloc_fail;
    out->bad_free = p->bad_free;
    out->spill_in = p->spill_in;
    irq_restore(state);
    return 0;
}

void pool_get_misc(pool_misc_t *out)
{
    uint32_t state = irq_save();
    *out = misc;
    irq_restore(state);
}

void pool_stats_reset(void)
{
    uint32_t state = irq_save();
    for (unsigned i = 0; i < NUM_POOLS; i++) {
        pools[i].peak = pools[i].in_use;
        pools[i].alloc_fail = pools[i].bad_free = pools[i].spill_in = 0;
    }
    misc.zero_size = misc.too_big = misc.unowned_free = 0;
    irq_restore(state);
}

uint32_t pool_total_bytes(void)
{
    uint32_t total = 0;
    for (unsigned i = 0; i < NUM_POOLS; i++)
        total += pools[i].block_size * pools[i].count;
    return total;
}

int pool_check(void)
{
    int rc = 0;
    uint32_t state = irq_save();

    for (unsigned i = 0; i < NUM_POOLS && rc == 0; i++) {
        const pool_t *p = &pools[i];
        uint32_t listed = 0;

        for (const free_node_t *n = p->free_head; n != 0; n = n->next) {
            uintptr_t a = (uintptr_t)n;
            if (listed >= p->fresh) { rc = 1; break; }                       /* cycle or too long */
            if (a < (uintptr_t)p->base || a >= (uintptr_t)p->base + (uintptr_t)p->count * p->block_size) { rc = 2; break; }
            if (((a - (uintptr_t)p->base) & (p->block_size - 1u)) != 0) { rc = 3; break; }
#if POOL_DEBUG
            {
                uint32_t idx = (uint32_t)((a - (uintptr_t)p->base) >> p->shift);
                if (p->bitmap[idx >> 5] & (1u << (idx & 31u))) { rc = 4; break; }   /* listed but marked allocated */
                for (uint32_t b = sizeof(free_node_t); b < p->block_size; b++)
                    if (((const uint8_t *)n)[b] != POISON_BYTE) { rc = 5; break; }   /* written after free */
                if (rc) break;
            }
#endif
            listed++;
        }
        if (rc == 0 && listed + p->in_use != p->fresh)
            rc = 6;                                                          /* blocks went missing */
    }

    irq_restore(state);
    return rc;
}
