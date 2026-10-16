#include "pool.h"
#include "pool_config.h"
#include "pool_port.h"

/* Where the pool memory goes. Default: an ordinary static array (the linker puts it in .bss).
   With -DPOOL_SECTION_NAME='".pool_mem"' it goes into that linker section instead, which lets the
   linker script show the pools' exact size and place them (the section must be NOLOAD there). */
#ifdef POOL_SECTION_NAME
#define POOL_SECTION __attribute__((section(POOL_SECTION_NAME)))
#else
#define POOL_SECTION
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
    uint32_t used;              /* bytes [0, used) have been handed out at least once (a multiple of the block size) */
    uint32_t in_use, peak, alloc_fail, bad_free, spill_in;
#if POOL_DEBUG
    uint32_t *bitmap;           /* 1 bit per block, 1 = currently allocated */
#endif
} __attribute__((aligned(64))) pool_t;  /* 64 bytes: &pools[i] is a shift, not a multiply */
_Static_assert(sizeof(pool_t) == 64, "pool_t must fit in 64 bytes");

/* ---- static storage generated from POOL_TABLE ---- */

/* All pools live in ONE arena, each pool starting on a POOL_OWNER_GRANULE boundary. That lets
 * pool_free() find the owner of a pointer with one table read: owner_map has one byte per
 * granule, holding the index of the pool that granule belongs to. (A pool whose size is not a
 * multiple of the granule leaves unused padding at its end; the granule still belongs to it.) */
#define GRAN ((size_t)POOL_OWNER_GRANULE)
_Static_assert(POOL_OWNER_GRANULE >= 8 && (POOL_OWNER_GRANULE & (POOL_OWNER_GRANULE - 1)) == 0,
               "POOL_OWNER_GRANULE must be a power of two >= 8");
#define ALIGN_UP_GRAN(n) (((size_t)(n) + GRAN - 1) & ~(GRAN - 1))
#define ADD_PADDED(size, count) + ALIGN_UP_GRAN((size_t)(size) * (count))
#define ARENA_BYTES (0 POOL_TABLE(ADD_PADDED))

static uint8_t arena[ARENA_BYTES] POOL_SECTION __attribute__((aligned(POOL_OWNER_GRANULE)));
static uint8_t owner_map[ARENA_BYTES / POOL_OWNER_GRANULE];

#if POOL_DEBUG
#define DEFINE_BITS(size, count) static uint32_t bits_##size[((count) + 31) / 32];
POOL_TABLE(DEFINE_BITS)
#endif

/* Compile-time checks on the table. */
#define CHECK_ENTRY(size, count) \
    _Static_assert((size) >= 8 && ((size) & ((size) - 1)) == 0, "pool block size must be a power of two >= 8"); \
    _Static_assert((size) <= 0x80000000u, "pool block size must be at most 2 GiB"); \
    _Static_assert((count) >= 1, "pool needs at least one block");
POOL_TABLE(CHECK_ENTRY)

#define ADD_BYTES(size, count) + ((size_t)(size) * (count))
_Static_assert((0 POOL_TABLE(ADD_BYTES)) <= POOL_MAX_TOTAL_BYTES, "the pools together exceed POOL_MAX_TOTAL_BYTES");

#define PLUS_ONE(size, count) + 1
#define NUM_POOLS (0 POOL_TABLE(PLUS_ONE))
_Static_assert(NUM_POOLS <= 255, "owner_map holds the pool index in one byte");

/* The biggest block (the table is strictly ascending, which pool_init() verifies). */
#define SIZE_ENTRY(size, count) (size),
static const uint32_t table_sizes[NUM_POOLS] = { POOL_TABLE(SIZE_ENTRY) };
#define MAX_BLOCK ((size_t)table_sizes[NUM_POOLS - 1])

static pool_t pools[NUM_POOLS];
static pool_misc_t misc;

/* route[k] = index of the smallest pool whose blocks hold at least 2^k bytes: a request of
 * `size` bytes goes to route[ceil_log2(size)]. Filled by pool_init(). */
static uint8_t route[32];

/* ---- small helpers ---- */

/* ceil(log2(size)), but at least 1; size must be >= 1. One CLZ instruction on the Cortex-M4. */
static inline unsigned ceil_log2(size_t size)
{
    return (unsigned)(sizeof(unsigned long) * 8u) - (unsigned)__builtin_clzl((unsigned long)((size - 1u) | 1u));
}

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
    p->used = 0;
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
    int rc = 0;
    /* Masked so an interrupt cannot allocate from a half-initialised pool. Calling this while
       blocks are outstanding makes the pool forget them: that is a caller error. */
    uint32_t state = pool_irq_save();

    unsigned i = 0;
    size_t off = 0;
#if POOL_DEBUG
#define INIT_ENTRY(size, count) \
    init_one(&pools[i], arena + off, (size), (count), bits_##size); \
    for (size_t g = 0; g < ALIGN_UP_GRAN((size_t)(size) * (count)) / GRAN; g++) owner_map[off / GRAN + g] = (uint8_t)i; \
    off += ALIGN_UP_GRAN((size_t)(size) * (count)); i++;
#else
#define INIT_ENTRY(size, count) \
    init_one(&pools[i], arena + off, (size), (count), 0); \
    for (size_t g = 0; g < ALIGN_UP_GRAN((size_t)(size) * (count)) / GRAN; g++) owner_map[off / GRAN + g] = (uint8_t)i; \
    off += ALIGN_UP_GRAN((size_t)(size) * (count)); i++;
#endif
    POOL_TABLE(INIT_ENTRY)

    for (unsigned k = 0; k < 32; k++) {
        unsigned j = 0;
        while (j + 1 < NUM_POOLS && pools[j].block_size < (1u << k))
            j++;
        route[k] = (uint8_t)j;
    }

    misc.zero_size = misc.too_big = misc.unowned_free = misc.corrupt_links = 0;

    for (unsigned k = 1; k < NUM_POOLS; k++)
        if (pools[k].block_size <= pools[k - 1].block_size)
            rc = -1;                                    /* must be strictly ascending */

    pool_irq_restore(state);
    return rc;
}

/* Bookkeeping for a block that has just been handed out. */
static inline __attribute__((always_inline)) void account(pool_t *p, const void *n, int lent)
{
    p->in_use++;
#if POOL_TRACK_PEAK
    if (p->in_use > p->peak)
        p->peak = p->in_use;
#endif
    if (lent)
        p->spill_in++;
#if POOL_DEBUG
    {
        uint32_t idx = (uint32_t)(((const uint8_t *)n - p->base) >> p->shift);
        p->bitmap[idx >> 5] |= 1u << (idx & 31u);
    }
#else
    (void)n;
#endif
}

/* The uncommon case: the free list is empty, so carve a block that was never used, or report
 * that the pool is exhausted. */
static void *take_fresh(pool_t *p, int lent)
{
    if (p->used >= (p->count << p->shift)) {
        if (!lent)
            p->alloc_fail++;
        return 0;
    }
    /* Every block is at least 8-byte aligned (the arena is aligned and block sizes are powers of
       two >= 8), so this cast is safe; going through void * tells the compiler we know. */
    free_node_t *n = (free_node_t *)(void *)(p->base + p->used);
    p->used += p->block_size;
    account(p, n, lent);
    return n;
}

/* Pop a freed block off `p`'s free list, or NULL if the list is empty. The caller holds the
 * critical section and then calls account(). */
static inline __attribute__((always_inline)) free_node_t *pop_free(pool_t *p)
{
    free_node_t *n = p->free_head;

#if POOL_DEBUG
    /* The link lives in memory the caller could have overwritten after freeing it (a bug
       in the caller). Do not follow a link that is outside the pool, misaligned, or points
       at a block that is marked allocated: abandon the list instead of writing through it. */
    if (n) {
        uintptr_t off = (uintptr_t)n - (uintptr_t)p->base;
        int sane = off < (uintptr_t)p->count * p->block_size
                && (off & (p->block_size - 1u)) == 0;
        if (sane) {
            uint32_t idx = (uint32_t)(off >> p->shift);
            sane = (p->bitmap[idx >> 5] & (1u << (idx & 31u))) == 0;
        }
        if (!sane) {
            misc.corrupt_links++;
            p->free_head = 0;
            n = 0;
        }
    }
#endif

    if (n)
        p->free_head = n->next;                         /* reuse a freed block */
    return n;
}

#if POOL_SPILL
/* Take one block from `p`, a larger pool lending to a smaller request, or NULL. */
static void *take_lent(pool_t *p)
{
    free_node_t *n = pop_free(p);
    if (n) {
        account(p, n, 1);
        return n;
    }
    return take_fresh(p, 1);
}
#endif

/* Everything pool_alloc() does when the best-fit pool's free list is empty. Out of line, and
 * given the saved interrupt state to restore, so the common path is a leaf that needs no stack
 * frame: it ends by jumping here instead of calling and coming back. */
static __attribute__((noinline)) void *alloc_slow(pool_t *p, uint32_t state)
{
    void *result = take_fresh(p, 0);
#if POOL_SPILL
    for (unsigned j = (unsigned)(p - pools) + 1; result == 0 && j < NUM_POOLS; j++)
        result = take_lent(&pools[j]);
#endif
    pool_irq_restore(state);
    return result;
}

/* The part of pool_alloc() that is the same whether the pool was found by size or by index. */
static inline __attribute__((always_inline)) void *alloc_from(pool_t *p, uint32_t state)
{
    free_node_t *n = pop_free(p);
    if (__builtin_expect(n != 0, 1)) {
        account(p, n, 0);
        pool_irq_restore(state);
        return n;
    }
    return alloc_slow(p, state);
}

void *pool_alloc(size_t size)
{
    uint32_t state = pool_irq_save();

    /* size - 1 wraps for 0, so one comparison rejects both 0 and "bigger than every pool". */
    if (__builtin_expect(size - 1u >= MAX_BLOCK, 0)) {
        if (size == 0)
            misc.zero_size++;
        else
            misc.too_big++;
        pool_irq_restore(state);
        return 0;
    }

    return alloc_from(&pools[route[ceil_log2(size)]], state);   /* smallest pool that fits: no search */
}

void *pool_alloc_class(size_t class_index)
{
    uint32_t state = pool_irq_save();

    if (__builtin_expect(class_index >= NUM_POOLS, 0)) {
        misc.too_big++;
        pool_irq_restore(state);
        return 0;
    }

    return alloc_from(&pools[class_index], state);
}

/* The pool whose granule contains `ptr`, or NULL outside the arena. For a pointer in the unused
 * tail of a pool's last granule this still answers that pool; find_owner() is the exact version. */
static inline pool_t *granule_owner(const void *ptr)
{
    size_t off = (size_t)((uintptr_t)ptr - (uintptr_t)arena);     /* wraps for pointers below the arena */
    if (off >= ARENA_BYTES)
        return 0;
    return &pools[owner_map[off / GRAN]];
}

static pool_t *find_owner(const void *ptr)
{
    pool_t *p = granule_owner(ptr);
    if (p && (uintptr_t)ptr - (uintptr_t)p->base >= ((uintptr_t)p->count << p->shift))
        return 0;                                                  /* padding after the pool's last block */
    return p;
}

/* pool_free() for a pointer it refuses. Out of line (and jumped to, not called) so the common
 * path stays short; `state` is the interrupt state to restore. */
static __attribute__((noinline)) void free_refused(pool_t *p, uint32_t off, uint32_t state)
{
    if (p == 0 || off >= (p->count << p->shift))
        misc.unowned_free++;                            /* outside every pool, or in the padding after one */
    else
        p->bad_free++;                                  /* middle of a block, never allocated, or double free */
    pool_irq_restore(state);
}

void pool_free(void *ptr)
{
    if (ptr == 0)
        return;

    uint32_t state = pool_irq_save();
    pool_t *p = granule_owner(ptr);
    if (__builtin_expect(p == 0, 0)) {
        free_refused(0, 0, state);
        return;
    }

    uint32_t off = (uint32_t)((uint8_t *)ptr - p->base);
    /* A block we handed out is at the start of a block (off is a multiple of the block size)
       and below `used`; anything else is refused. */
    if (__builtin_expect(((off & (p->block_size - 1u)) | (off >= p->used)) != 0, 0)) {
        free_refused(p, off, state);
        return;
    }
#if POOL_DEBUG
    {
        uint32_t idx = off >> p->shift;
        uint32_t bit = 1u << (idx & 31u);
        if ((p->bitmap[idx >> 5] & bit) == 0) {          /* already free: double free */
            free_refused(p, off, state);
            return;
        }
        p->bitmap[idx >> 5] &= ~bit;
        poison((uint8_t *)ptr, p->block_size);
    }
#endif
    free_node_t *n = (free_node_t *)ptr;
    n->next = p->free_head;
    p->free_head = n;
    p->in_use--;
    pool_irq_restore(state);
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
    if (index >= NUM_POOLS || out == 0)
        return -1;
    uint32_t state = pool_irq_save();
    const pool_t *p = &pools[index];
    out->block_size = p->block_size;
    out->count = p->count;
    out->in_use = p->in_use;
    out->peak = p->peak;
    out->alloc_fail = p->alloc_fail;
    out->bad_free = p->bad_free;
    out->spill_in = p->spill_in;
    pool_irq_restore(state);
    return 0;
}

void pool_get_misc(pool_misc_t *out)
{
    if (out == 0)
        return;
    uint32_t state = pool_irq_save();
    *out = misc;
    pool_irq_restore(state);
}

void pool_stats_reset(void)
{
    uint32_t state = pool_irq_save();
    for (unsigned i = 0; i < NUM_POOLS; i++) {
        pools[i].peak = pools[i].in_use;
        pools[i].alloc_fail = pools[i].bad_free = pools[i].spill_in = 0;
    }
    misc.zero_size = misc.too_big = misc.unowned_free = misc.corrupt_links = 0;
    pool_irq_restore(state);
}

uint32_t pool_total_bytes(void)
{
    uint32_t total = 0;
    for (unsigned i = 0; i < NUM_POOLS; i++)
        total += pools[i].block_size * pools[i].count;
    return total;
}

/* Check one pool; the caller holds the critical section. */
static int check_one(const pool_t *p)
{
    uint32_t listed = 0;
    uint32_t fresh = p->used >> p->shift;               /* blocks handed out at least once */

    for (const free_node_t *n = p->free_head; n != 0; n = n->next) {
        uintptr_t a = (uintptr_t)n;
        if (listed >= fresh)
            return 1;                                                    /* cycle or too long */
        if (a < (uintptr_t)p->base || a >= (uintptr_t)p->base + (uintptr_t)p->count * p->block_size)
            return 2;                                                    /* link leaves the pool */
        if (((a - (uintptr_t)p->base) & (p->block_size - 1u)) != 0)
            return 3;                                                    /* link not at a block start */
#if POOL_DEBUG
        {
            uint32_t idx = (uint32_t)((a - (uintptr_t)p->base) >> p->shift);
            if (p->bitmap[idx >> 5] & (1u << (idx & 31u)))
                return 4;                                                /* listed but marked allocated */
            for (uint32_t b = sizeof(free_node_t); b < p->block_size; b++)
                if (((const uint8_t *)n)[b] != POISON_BYTE)
                    return 5;                                            /* written after free */
        }
#endif
        listed++;
    }
    if (listed + p->in_use != fresh)
        return 6;                                                        /* blocks went missing */
    return 0;
}

int pool_check(void)
{
    /* One pool per critical section, so interrupts are masked for at most one pool's walk at
       a time. It is still a debugging call, linear in the number of free blocks. */
    for (unsigned i = 0; i < NUM_POOLS; i++) {
        uint32_t state = pool_irq_save();
        int rc = check_one(&pools[i]);
        pool_irq_restore(state);
        if (rc)
            return rc;
    }
    return 0;
}
