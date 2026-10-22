/* afalloc2: see afalloc.h and README.md.
 *
 * Layout in one picture:
 *
 *   reserved heap (PROT_NONE until used):  [slab 0][slab 1][slab 2]...      each slab 256 KiB, aligned
 *   metadata (separate mapping):           [meta 0][meta 1][meta 2]...      one slab_meta_t per slab
 *   a slab serves ONE size class: its blocks are carved lazily, a free list threads the freed ones
 *
 * A thread owns the slabs it allocates from, so the fast path touches only thread-local state and the
 * slab's metadata. A block freed by another thread is pushed on the slab's lock-free "remote" list and
 * picked up by the owner. Blocks above 32 KiB are mapped directly with guard pages.
 */
#define _GNU_SOURCE
#include "afalloc.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define COLD __attribute__((noinline, cold))
#define NOINLINE __attribute__((noinline))

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

/* ---------------------------------------------------------------- constants */
#define SLAB_SHIFT 18
#define SLAB_SIZE ((size_t)1 << SLAB_SHIFT)         /* 256 KiB */
#define SMALL_MAX 32768u
#define NCLASS 40
#define BITMAP_WORDS ((SLAB_SIZE / 16) / 64)        /* enough bits for 16-byte blocks */
#define QUARANTINE 32
#define ZERO_ON_FREE_MAX 1024u
#define KEEP_FREE_SLABS 256                         /* empty slabs kept hot (64 MiB) before MADV_FREE */
#define LCACHE_SLOTS 32                             /* freed large mappings kept for reuse */
#define LCACHE_MAX_BYTES ((size_t)64 << 20)
#define LCACHE_MAX_ENTRY ((size_t)8 << 20)
#define LARGE_SLOTS (1u << 15)
#define HEAP_ARENA_BYTES ((size_t)8 << 20)

#define FLAG_HARD 1u                                /* level >= 1: validate */
#define FLAG_EXACT 2u                               /* level 2: allocation bitmap */

typedef struct slab_meta slab_meta_t;
typedef struct heap heap_t;

/* One per slab, in a mapping separate from the blocks. The first group is touched only by the owning
   thread; the second (own cache lines) is the cross-thread part, so remote frees do not bounce the
   owner's lines. */
struct slab_meta {
    void *local_free;               /* head of the free list (owner only) */
    uint8_t *data;
    uint64_t magic;                 /* ceil(2^40 / block_size): offset / block_size without a division */
    uintptr_t key;                  /* per-slab key for the free-list links */
    uint32_t block_size;
    uint32_t cap;                   /* blocks that fit */
    uint32_t fresh;                 /* blocks carved so far */
    uint32_t used;                  /* blocks handed out and not yet collected back (owner only) */
    uint32_t stride, start;         /* level 2: order in which fresh blocks are carved */
    uint64_t *bits;                 /* level 2: 1 = allocated */
    slab_meta_t *all_next, *all_prev;       /* every slab of the owner */
    slab_meta_t *avail_next, *avail_prev;   /* owner's slabs that have free blocks */
    slab_meta_t *ab_next, *ab_prev;         /* abandoned list (global lock) */
    uint32_t ab_live;                       /* abandoned slab: blocks still allocated (global lock) */
    uint8_t ab_listed;                      /* abandoned slab is on the list (global lock) */
    uint8_t state;                  /* 0 free, 1 small */
    uint8_t cls;
    uint8_t flags;
    uint8_t zero_on_free;

    _Alignas(128) _Atomic(uintptr_t) remote_free;   /* blocks freed by other threads */
    _Atomic(heap_t *) owner;
    _Atomic(slab_meta_t *) pending_next;
    _Atomic(uint8_t) in_pending;
    _Atomic(uint8_t) linked;        /* the owner will look at this slab (current or on its avail list) */
};

struct heap {
    slab_meta_t *cur[NCLASS];
    slab_meta_t *avail[NCLASS];
    slab_meta_t *all;
    uint8_t empty_kept[NCLASS];                     /* empty slabs held back per class (hysteresis) */
    uint64_t rng[2];
    heap_t *next_free;
    void *quarantine[QUARANTINE];
    unsigned qpos;
    _Alignas(128) _Atomic(slab_meta_t *) pending;   /* slabs that received remote frees while full */
};

typedef struct {
    uintptr_t user, map;
    size_t map_len, usable;
} large_ent_t;
#define LARGE_EMPTY 0
#define LARGE_TOMB 1

static struct {
    atomic_int ready;
    int failed;
    int level, level_req;
    size_t page;
    uint8_t *base;
    size_t reserve;
    uint32_t nslabs;
    slab_meta_t *meta;
    uint32_t *freestack;
    uint64_t *bits;
    uint32_t next_fresh, nfree;
    pthread_mutex_t lock;
    slab_meta_t *abandoned[NCLASS];
    heap_t *heap_free;
    uint8_t *heap_arena, *heap_arena_end;
    pthread_key_t key;
    large_ent_t *large;
    size_t large_live, large_bytes;
    size_t slabs_in_use;
    large_ent_t lcache[LCACHE_SLOTS];
    size_t lcache_bytes;
    unsigned lcache_next;
} G = { .level_req = -1, .lock = PTHREAD_MUTEX_INITIALIZER };

static void af_thread_exit_(void *arg);
static pthread_once_t once_ctl = PTHREAD_ONCE_INIT;
static _Thread_local heap_t *tls_h;
static slab_meta_t g_empty;                         /* "no slab": local_free NULL, cap 0 */
static uint32_t class_size[NCLASS];

/* ---------------------------------------------------------------- helpers */
static COLD __attribute__((noreturn)) void die(const char *msg)
{
    static const char pre[] = "afalloc: ";
    ssize_t r = write(2, pre, sizeof pre - 1);
    r = write(2, msg, strlen(msg));
    r = write(2, "\n", 1);
    (void)r;
    abort();
}

static void fill_random(void *buf, size_t n)
{
#ifdef __APPLE__
    arc4random_buf(buf, n);
#else
    uint8_t *p = buf;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r <= 0) break;
        p += r;
        n -= (size_t)r;
    }
#endif
}

static inline uint64_t rng_next(heap_t *h)          /* xorshift128+ */
{
    uint64_t s1 = h->rng[0], s0 = h->rng[1];
    h->rng[0] = s0;
    s1 ^= s1 << 23;
    h->rng[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
    return h->rng[1] + s0;
}

static void init_classes(void)
{
    for (unsigned c = 0; c < 8; c++)
        class_size[c] = (c + 1) * 16;
    for (unsigned c = 8; c < NCLASS; c++) {
        unsigned b = 7 + ((c - 8) >> 2), j = (c - 8) & 3;
        class_size[c] = (1u << b) + (j + 1) * (1u << (b - 2));
    }
}

/* size class of n bytes, 0 < n <= SMALL_MAX (n == 0 maps to the smallest) */
static inline unsigned size_class(size_t n)
{
    if (n <= 128)
        return n ? (unsigned)((n + 15) >> 4) - 1 : 0;
    unsigned b = 63 - (unsigned)__builtin_clzll(n - 1);
    return 8 + (b - 7) * 4 + (unsigned)(((n - 1) - ((size_t)1 << b)) >> (b - 2));
}

static void *map_guarded(size_t size)               /* read/write mapping with a guard page on each side */
{
    size_t pg = G.page;
    size = (size + pg - 1) & ~(pg - 1);
    uint8_t *p = mmap(NULL, size + 2 * pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    if (mprotect(p + pg, size, PROT_READ | PROT_WRITE) != 0) {
        munmap(p, size + 2 * pg);
        return NULL;
    }
    return p + pg;
}

/* ---------------------------------------------------------------- global init */
static void global_init(void)
{
    G.page = (size_t)sysconf(_SC_PAGESIZE);
    init_classes();
    int level = G.level_req;
    if (level < 0) {
        const char *e = getenv("AFALLOC_SECURE");
        level = (e && e[0] >= '0' && e[0] <= '2' && !e[1]) ? e[0] - '0' : 1;
    }
    G.level = level;

    size_t tries[] = { (size_t)16 << 30, (size_t)4 << 30, (size_t)1 << 30, (size_t)256 << 20 };
    uint8_t *raw = NULL;
    size_t reserve = 0;
    for (unsigned i = 0; i < sizeof tries / sizeof *tries && !raw; i++) {
        void *p = mmap(NULL, tries[i] + SLAB_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p != MAP_FAILED) {
            raw = p;
            reserve = tries[i];
        }
    }
    if (!raw) { G.failed = 1; return; }
    G.base = (uint8_t *)(((uintptr_t)raw + SLAB_SIZE - 1) & ~(uintptr_t)(SLAB_SIZE - 1));
    G.reserve = reserve;
    G.nslabs = (uint32_t)(reserve >> SLAB_SHIFT);

    G.meta = map_guarded((size_t)G.nslabs * sizeof(slab_meta_t));
    G.freestack = map_guarded((size_t)G.nslabs * sizeof(uint32_t));
    G.heap_arena = map_guarded(HEAP_ARENA_BYTES);
    G.large = map_guarded(LARGE_SLOTS * sizeof(large_ent_t));
    if (level == 2)
        G.bits = map_guarded((size_t)G.nslabs * BITMAP_WORDS * 8);
    if (!G.meta || !G.freestack || !G.heap_arena || !G.large || (level == 2 && !G.bits)) { G.failed = 1; return; }
    G.heap_arena_end = G.heap_arena + HEAP_ARENA_BYTES;
    pthread_key_create(&G.key, af_thread_exit_);
    atomic_store_explicit(&G.ready, 1, memory_order_release);
}

int afset_security(int level)
{
    if (level < 0 || level > 2 || atomic_load(&G.ready))
        return -1;
    G.level_req = level;
    return 0;
}

/* ---------------------------------------------------------------- slab pool (global lock) */
static inline size_t guard_bytes(void) { return G.level >= 1 ? G.page : 0; }

static slab_meta_t *slab_acquire(void)
{
    pthread_mutex_lock(&G.lock);
    uint32_t idx;
    bool fresh = false;
    if (G.nfree) {
        idx = G.freestack[--G.nfree];
    } else if (G.next_fresh < G.nslabs) {
        idx = G.next_fresh++;
        fresh = true;
    } else {
        pthread_mutex_unlock(&G.lock);
        return NULL;
    }
    G.slabs_in_use++;
    if (fresh) {
        uint8_t *d = G.base + ((size_t)idx << SLAB_SHIFT);
        if (mprotect(d, SLAB_SIZE - guard_bytes(), PROT_READ | PROT_WRITE) != 0) {
            G.next_fresh--;
            G.slabs_in_use--;
            pthread_mutex_unlock(&G.lock);
            return NULL;
        }
    }
    pthread_mutex_unlock(&G.lock);
    return &G.meta[idx];
}

static void slab_return_locked(slab_meta_t *m)      /* G.lock held */
{
    uint32_t idx = (uint32_t)(m - G.meta);
    m->state = 0;
    if (G.nfree >= KEEP_FREE_SLABS)
        madvise(m->data, SLAB_SIZE - guard_bytes(), MADV_FREE);
    G.freestack[G.nfree++] = idx;
    G.slabs_in_use--;
}

static void slab_return(slab_meta_t *m)
{
    pthread_mutex_lock(&G.lock);
    slab_return_locked(m);
    pthread_mutex_unlock(&G.lock);
}

/* abandoned list (G.lock held) */
static void ab_push(slab_meta_t *m)
{
    m->ab_prev = NULL;
    m->ab_next = G.abandoned[m->cls];
    if (m->ab_next)
        m->ab_next->ab_prev = m;
    G.abandoned[m->cls] = m;
    m->ab_listed = 1;
}
static void ab_remove(slab_meta_t *m)
{
    if (m->ab_prev)
        m->ab_prev->ab_next = m->ab_next;
    else
        G.abandoned[m->cls] = m->ab_next;
    if (m->ab_next)
        m->ab_next->ab_prev = m->ab_prev;
    m->ab_listed = 0;
}

/* ---------------------------------------------------------------- heaps */
static heap_t *heap_create(void)
{
    pthread_once(&once_ctl, global_init);
    if (G.failed)
        return NULL;
    heap_t *h;
    pthread_mutex_lock(&G.lock);
    if (G.heap_free) {
        h = G.heap_free;
        G.heap_free = h->next_free;
    } else if ((size_t)(G.heap_arena_end - G.heap_arena) >= sizeof(heap_t)) {
        h = (heap_t *)G.heap_arena;
        G.heap_arena += (sizeof(heap_t) + 127) & ~(size_t)127;
    } else {
        h = NULL;
    }
    pthread_mutex_unlock(&G.lock);
    if (!h)
        return NULL;
    memset(h, 0, sizeof *h);
    for (unsigned c = 0; c < NCLASS; c++)
        h->cur[c] = &g_empty;
    fill_random(h->rng, sizeof h->rng);
    if (!h->rng[0] && !h->rng[1])
        h->rng[0] = 88172645463325252ull;
    tls_h = h;
    pthread_setspecific(G.key, h);
    return h;
}

static inline heap_t *get_heap(void)
{
    heap_t *h = tls_h;
    return likely(h != NULL) ? h : heap_create();
}

/* ---------------------------------------------------------------- slab lists */
static inline void all_push(heap_t *h, slab_meta_t *m)
{
    m->all_prev = NULL;
    m->all_next = h->all;
    if (h->all)
        h->all->all_prev = m;
    h->all = m;
}
static inline void all_remove(heap_t *h, slab_meta_t *m)
{
    if (m->all_prev)
        m->all_prev->all_next = m->all_next;
    else
        h->all = m->all_next;
    if (m->all_next)
        m->all_next->all_prev = m->all_prev;
}
static inline void avail_push(heap_t *h, slab_meta_t *m)
{
    unsigned c = m->cls;
    m->avail_prev = NULL;
    m->avail_next = h->avail[c];
    if (h->avail[c])
        h->avail[c]->avail_prev = m;
    h->avail[c] = m;
}
static inline void avail_remove(heap_t *h, slab_meta_t *m)
{
    unsigned c = m->cls;
    if (m->avail_prev)
        m->avail_prev->avail_next = m->avail_next;
    else
        h->avail[c] = m->avail_next;
    if (m->avail_next)
        m->avail_next->avail_prev = m->avail_prev;
}

/* ---------------------------------------------------------------- block bookkeeping */
static inline uint32_t block_index(const slab_meta_t *m, uint32_t k)
{
    return m->stride ? (uint32_t)(((uint64_t)m->start + (uint64_t)k * m->stride) % m->cap) : k;
}

static inline bool link_ok(const slab_meta_t *m, uintptr_t blk, uintptr_t next)
{
    if (next == 0)
        return true;
    return (next - (uintptr_t)m->data) < (uintptr_t)m->cap * m->block_size && ((next ^ blk) >> SLAB_SHIFT) == 0
        && (next & 15) == 0;
}

static inline void bitmap_set(slab_meta_t *m, const void *p)
{
    uint32_t i = (uint32_t)((((uintptr_t)p - (uintptr_t)m->data) * m->magic) >> 40);
    atomic_fetch_or_explicit((_Atomic uint64_t *)&m->bits[i >> 6], (uint64_t)1 << (i & 63), memory_order_relaxed);
}

/* returns false if the bit was already clear (double free / never allocated) */
static inline bool bitmap_clear(slab_meta_t *m, const void *p)
{
    uint32_t i = (uint32_t)((((uintptr_t)p - (uintptr_t)m->data) * m->magic) >> 40);
    uint64_t bit = (uint64_t)1 << (i & 63);
    uint64_t old = atomic_fetch_and_explicit((_Atomic uint64_t *)&m->bits[i >> 6], ~bit, memory_order_relaxed);
    return (old & bit) != 0;
}

static void extend(slab_meta_t *m)                  /* thread a batch of fresh blocks onto the free list */
{
    uint32_t batch = 4096u / m->block_size;
    if (batch < 1) batch = 1;
    if (batch > 64) batch = 64;
    uint32_t k = m->cap - m->fresh;
    if (k > batch) k = batch;
    uint32_t i0 = m->fresh;
    m->fresh += k;
    uintptr_t next = 0;
    for (uint32_t j = k; j-- > 0;) {                /* lowest address first */
        uint8_t *b = m->data + (size_t)block_index(m, i0 + j) * m->block_size;
        *(uintptr_t *)b = next ^ m->key;
        next = (uintptr_t)b;
    }
    m->local_free = (void *)next;
}

static void slab_init(heap_t *h, slab_meta_t *m, unsigned c)
{
    uint32_t idx = (uint32_t)(m - G.meta);
    uint32_t size = class_size[c];
    m->state = 1;
    m->cls = (uint8_t)c;
    m->block_size = size;
    m->data = G.base + ((size_t)idx << SLAB_SHIFT);
    m->cap = (uint32_t)((SLAB_SIZE - guard_bytes()) / size);
    m->magic = (((uint64_t)1 << 40) + size - 1) / size;
    m->fresh = 0;
    m->used = 0;
    m->local_free = NULL;
    m->flags = (uint8_t)(G.level >= 1 ? FLAG_HARD : 0) | (uint8_t)(G.level == 2 ? FLAG_EXACT : 0);
    m->zero_on_free = (G.level == 2 && size <= ZERO_ON_FREE_MAX) ? 1 : 0;
    m->key = (G.level >= 1) ? (uintptr_t)rng_next(h) : 0;
    m->stride = 0;
    m->start = 0;
    if (G.level == 2) {
        m->bits = &G.bits[(size_t)idx * BITMAP_WORDS];
        memset(m->bits, 0, (((size_t)m->cap + 63) / 64) * 8);
        static const uint32_t primes[] = { 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97, 101 };
        uint32_t s = 0;
        for (unsigned t = 0; t < 16 && !s; t++) {
            uint32_t p = primes[(rng_next(h) >> 8) % 16];
            if (m->cap % p != 0 && m->cap > 1) s = p;
        }
        m->stride = s;                              /* 0 (sequential) only if no prime fits */
        m->start = m->cap ? (uint32_t)((rng_next(h) >> 8) % m->cap) : 0;
    }
    atomic_store_explicit(&m->remote_free, 0, memory_order_relaxed);
    atomic_store_explicit(&m->in_pending, 0, memory_order_relaxed);
    atomic_store_explicit(&m->linked, 0, memory_order_relaxed);
    atomic_store_explicit(&m->owner, h, memory_order_release);
    all_push(h, m);
}

/* Owner: take everything other threads freed in this slab and put it on the local list. */
static void collect_remote(slab_meta_t *m)
{
    uintptr_t head = atomic_exchange_explicit(&m->remote_free, 0, memory_order_acquire);
    if (!head)
        return;
    uint32_t n = 0;
    uintptr_t b = head, next;
    for (;;) {
        n++;
        next = *(uintptr_t *)b ^ m->key;
        if ((m->flags & FLAG_HARD) && !link_ok(m, b, next))
            die("corrupted remote free list");
        if (!next)
            break;
        b = next;
    }
    if (n > m->used)
        die("corrupted heap: more blocks freed than were allocated");
    *(uintptr_t *)b = (uintptr_t)m->local_free ^ m->key;
    m->local_free = (void *)head;
    m->used -= n;
}

static void slab_release(heap_t *h, slab_meta_t *m)    /* m is empty and not current / on avail list */
{
    all_remove(h, m);
    atomic_store_explicit(&m->owner, NULL, memory_order_relaxed);
    slab_return(m);
}

static void drain_pending(heap_t *h);

/* m is empty, not current and not on an avail list. If a stale notification still points at it, process
   the notifications first: they may release it themselves. */
static void release_checked(heap_t *h, slab_meta_t *m)
{
    if (atomic_load_explicit(&m->in_pending, memory_order_relaxed)) {
        drain_pending(h);
        if (m->state != 1)
            return;
    }
    slab_release(h, m);
}

static void drain_pending(heap_t *h)
{
    slab_meta_t *s = atomic_exchange_explicit(&h->pending, NULL, memory_order_acquire);
    while (s) {
        slab_meta_t *next = atomic_load_explicit(&s->pending_next, memory_order_relaxed);
        atomic_store_explicit(&s->in_pending, 0, memory_order_release);
        if (atomic_load_explicit(&s->owner, memory_order_relaxed) == h && s->state == 1) {
            collect_remote(s);
            if (!atomic_load_explicit(&s->linked, memory_order_relaxed)) {
                if (s->used == 0) {
                    slab_release(h, s);
                } else if (s->local_free || s->fresh < s->cap) {
                    atomic_store_explicit(&s->linked, 1, memory_order_relaxed);
                    avail_push(h, s);
                }
            }
        }
        s = next;
    }
}

/* ---------------------------------------------------------------- small allocation, slow path */
static slab_meta_t *next_slab(heap_t *h, unsigned c)
{
    slab_meta_t *s;
    while ((s = h->avail[c]) != NULL) {                 /* 1. a partly used slab of ours */
        avail_remove(h, s);
        if (s->used == 0 && h->empty_kept[c])
            h->empty_kept[c]--;
        collect_remote(s);
        if (s->local_free || s->fresh < s->cap)
            return s;
        atomic_store_explicit(&s->linked, 0, memory_order_relaxed);
    }
    for (;;) {                                          /* 2. a slab abandoned by an exited thread */
        pthread_mutex_lock(&G.lock);
        s = G.abandoned[c];
        if (s) {
            ab_remove(s);
            atomic_store_explicit(&s->owner, h, memory_order_release);   /* under the lock: no free sees a half-adopted slab */
        }
        pthread_mutex_unlock(&G.lock);
        if (!s)
            break;
        all_push(h, s);
        collect_remote(s);
        if (s->used == 0) {
            slab_release(h, s);
            continue;
        }
        if (s->local_free || s->fresh < s->cap)
            return s;
        atomic_store_explicit(&s->linked, 0, memory_order_relaxed);
    }
    s = slab_acquire();                                 /* 3. a new slab */
    if (!s)
        return NULL;
    slab_init(h, s, c);
    return s;
}

static COLD void *small_alloc_slow(heap_t *h, unsigned c)
{
    if (atomic_load_explicit(&h->pending, memory_order_relaxed))
        drain_pending(h);
    slab_meta_t *m = h->cur[c];
    for (;;) {
        if (m != &g_empty) {
            if (!m->local_free) {
                collect_remote(m);
                if (!m->local_free && m->fresh < m->cap)
                    extend(m);
            }
            if (!m->local_free) {                       /* full: leave the working set */
                atomic_store_explicit(&m->linked, 0, memory_order_relaxed);
                atomic_thread_fence(memory_order_seq_cst);
                if (atomic_load_explicit(&m->remote_free, memory_order_relaxed)) {
                    atomic_store_explicit(&m->linked, 1, memory_order_relaxed);
                    continue;                           /* a remote free slipped in: collect it */
                }
                h->cur[c] = &g_empty;
                m = &g_empty;
                continue;
            }
            uintptr_t b = (uintptr_t)m->local_free;
            uintptr_t nx = *(uintptr_t *)b ^ m->key;
            if ((m->flags & FLAG_HARD) && !link_ok(m, b, nx))
                die("corrupted free list (write after free?)");
            m->local_free = (void *)nx;
            m->used++;
            if (m->flags & FLAG_EXACT) {
                bitmap_set(m, (void *)b);
                *(uintptr_t *)b = 0;
            }
            return (void *)b;
        }
        m = next_slab(h, c);
        if (!m)
            return NULL;
        atomic_store_explicit(&m->linked, 1, memory_order_relaxed);
        h->cur[c] = m;
    }
}

static inline void *small_alloc(heap_t *h, unsigned c)
{
    slab_meta_t *m = h->cur[c];
    uintptr_t b = (uintptr_t)m->local_free;
    if (likely(b != 0)) {
        uintptr_t nx = *(uintptr_t *)b ^ m->key;
        if (unlikely(m->flags != 0)) {
            if (!link_ok(m, b, nx))
                die("corrupted free list (write after free?)");
            if (m->flags & FLAG_EXACT) {
                bitmap_set(m, (void *)b);
                *(uintptr_t *)b = 0;
            }
        }
        m->local_free = (void *)nx;
        m->used++;
        return (void *)b;
    }
    return small_alloc_slow(h, c);
}

/* ---------------------------------------------------------------- small free */
static COLD void free_slow(heap_t *h, slab_meta_t *m)
{
    unsigned c = m->cls;
    if (!atomic_load_explicit(&m->linked, memory_order_relaxed)) {
        /* the slab was full: it can serve allocations again (or is empty) */
        if (m->used == 0 && h->empty_kept[c] >= 1) {
            release_checked(h, m);
        } else {
            if (m->used == 0)
                h->empty_kept[c]++;
            atomic_store_explicit(&m->linked, 1, memory_order_relaxed);
            avail_push(h, m);
        }
    } else if (m->used == 0 && m != h->cur[c]) {
        if (h->empty_kept[c] >= 1) {
            avail_remove(h, m);
            atomic_store_explicit(&m->linked, 0, memory_order_relaxed);
            release_checked(h, m);
        } else {
            h->empty_kept[c]++;                         /* keep one empty slab per class: no thrash at the boundary */
        }
    }
}

static inline void block_push(heap_t *h, slab_meta_t *m, void *p)       /* the real free, no checks */
{
    *(uintptr_t *)p = (uintptr_t)m->local_free ^ m->key;
    m->local_free = p;
    if (unlikely(--m->used == 0 || !atomic_load_explicit(&m->linked, memory_order_relaxed)))
        free_slow(h, m);
}

/* level >= 1: is p the start of a block of this slab? */
static inline void check_block(const slab_meta_t *m, const void *p)
{
    uintptr_t off = (uintptr_t)p - (uintptr_t)m->data;
    uint32_t i = (uint32_t)((off * m->magic) >> 40);
    if (m->state != 1 || off >= (uintptr_t)m->cap * m->block_size || (uintptr_t)i * m->block_size != off)
        die("free of an invalid pointer (not the start of a block)");
}

static COLD void free_checked_local(heap_t *h, slab_meta_t *m, void *p)
{
    check_block(m, p);
    if (p == m->local_free)
        die("double free detected");
    if (m->flags & FLAG_EXACT) {
        if (!bitmap_clear(m, p))
            die("double free or free of a block that is not allocated");
        if (m->zero_on_free)
            memset(p, 0, m->block_size);
        void *old = h->quarantine[h->qpos];
        h->quarantine[h->qpos] = p;
        h->qpos = (h->qpos + 1) % QUARANTINE;
        if (old) {                                  /* the oldest entry is released, after a check */
            slab_meta_t *om = &G.meta[((uintptr_t)old - (uintptr_t)G.base) >> SLAB_SHIFT];
            if (om->zero_on_free) {
                const uint64_t *w = (const uint64_t *)old;
                for (uint32_t i = 1; i < om->block_size / 8; i++)
                    if (w[i])
                        die("write after free detected (a freed block was modified)");
            }
            if (atomic_load_explicit(&om->owner, memory_order_relaxed) == h) {
                block_push(h, om, old);
            } else {
                /* its slab changed hands while it sat in quarantine: free it the remote way */
                uintptr_t head = atomic_load_explicit(&om->remote_free, memory_order_relaxed);
                do {
                    *(uintptr_t *)old = head ^ om->key;
                } while (!atomic_compare_exchange_weak_explicit(&om->remote_free, &head, (uintptr_t)old,
                                                                  memory_order_release, memory_order_relaxed));
            }
        }
        return;
    }
    block_push(h, m, p);
}

/* A block of an abandoned slab (its owner thread exited). Everything happens under the global lock, so the
   slab cannot be adopted or reused while the count is updated; the free that takes the last live block
   returns the whole slab to the pool. Returns false if the slab was adopted in the meantime. */
static bool abandoned_free(slab_meta_t *m, void *p)
{
    pthread_mutex_lock(&G.lock);
    if (atomic_load_explicit(&m->owner, memory_order_relaxed) != NULL || !m->ab_listed) {
        pthread_mutex_unlock(&G.lock);
        return false;
    }
    if (m->ab_live == 0) {
        pthread_mutex_unlock(&G.lock);
        die("double free detected (the slab has no live blocks)");
    }
    uintptr_t head = atomic_load_explicit(&m->remote_free, memory_order_relaxed);
    do {                                            /* other threads may still push without the lock */
        *(uintptr_t *)p = head ^ m->key;
    } while (!atomic_compare_exchange_weak_explicit(&m->remote_free, &head, (uintptr_t)p, memory_order_release,
                                                      memory_order_relaxed));
    if (--m->ab_live == 0) {
        ab_remove(m);
        slab_return_locked(m);
    }
    pthread_mutex_unlock(&G.lock);
    return true;
}

/* a block freed by a thread that does not own the slab */
static NOINLINE void small_free_remote(slab_meta_t *m, void *p)
{
    if (m->flags & FLAG_HARD) {
        check_block(m, p);
        if (m->flags & FLAG_EXACT) {
            if (!bitmap_clear(m, p))
                die("double free or free of a block that is not allocated");
            if (m->zero_on_free)
                memset(p, 0, m->block_size);
        }
    }
    if (unlikely(atomic_load_explicit(&m->owner, memory_order_acquire) == NULL) && abandoned_free(m, p))
        return;
    uintptr_t head = atomic_load_explicit(&m->remote_free, memory_order_relaxed);
    do {
        *(uintptr_t *)p = head ^ m->key;
    } while (!atomic_compare_exchange_weak_explicit(&m->remote_free, &head, (uintptr_t)p, memory_order_release,
                                                      memory_order_relaxed));
    atomic_thread_fence(memory_order_seq_cst);
    if (unlikely(!atomic_load_explicit(&m->linked, memory_order_relaxed))) {
        heap_t *o = atomic_load_explicit(&m->owner, memory_order_acquire);
        uint8_t zero = 0;
        if (o && atomic_compare_exchange_strong_explicit(&m->in_pending, &zero, 1, memory_order_acq_rel,
                                                         memory_order_relaxed)) {
            slab_meta_t *old = atomic_load_explicit(&o->pending, memory_order_relaxed);
            do {
                atomic_store_explicit(&m->pending_next, old, memory_order_relaxed);
            } while (!atomic_compare_exchange_weak_explicit(&o->pending, &old, m, memory_order_release,
                                                              memory_order_relaxed));
        }
    }
}

/* ---------------------------------------------------------------- large blocks (direct mappings) */
static inline size_t large_hash(uintptr_t p) { return (size_t)((p >> 12) * 0x9E3779B97F4A7C15ull >> 40) & (LARGE_SLOTS - 1); }

/* Round a large request up to a quarter-power-of-two step (at most 25% of address space, which costs no
   memory until touched) so freed mappings are likely to fit the next request exactly. */
static inline size_t large_round(size_t n, size_t pg)
{
    unsigned b = 63 - (unsigned)__builtin_clzll(n - 1);        /* n > SMALL_MAX: b >= 15 */
    size_t r = (((n - 1) >> (b - 2)) + 1) << (b - 2);
    return (r + pg - 1) & ~(pg - 1);
}

static void large_insert(uintptr_t user, uintptr_t map, size_t total, size_t data)    /* lock held */
{
    size_t i = large_hash(user);
    while (G.large[i].user > LARGE_TOMB)
        i = (i + 1) & (LARGE_SLOTS - 1);
    G.large[i] = (large_ent_t){ user, map, total, data };
    G.large_live++;
    G.large_bytes += total;
}

/* *from_cache is set when the pages may hold old data (calloc must clear them) */
static void *large_alloc(size_t n, size_t align, bool *from_cache)
{
    size_t pg = G.page, guard = G.level >= 1 ? pg : 0;
    if (n > ((size_t)1 << 46))
        return NULL;
    size_t data = (align > pg || n <= SMALL_MAX) ? (n + pg - 1) & ~(pg - 1) : large_round(n, pg);
    if (data == 0)
        data = pg;
    if (align <= pg && data <= LCACHE_MAX_ENTRY) {              /* a cached mapping of exactly this size? */
        pthread_mutex_lock(&G.lock);
        for (unsigned i = 0; i < LCACHE_SLOTS; i++) {
            large_ent_t *e = &G.lcache[i];
            if (e->user && e->usable == data) {
                large_ent_t hit = *e;
                e->user = 0;
                G.lcache_bytes -= hit.map_len;
                large_insert(hit.user, hit.map, hit.map_len, hit.usable);
                pthread_mutex_unlock(&G.lock);
                if (from_cache)
                    *from_cache = true;
                return (void *)hit.user;
            }
        }
        pthread_mutex_unlock(&G.lock);
    }
    size_t slack = align > pg ? align : 0;
    size_t total = data + 2 * guard + slack;
    uint8_t *map = mmap(NULL, total, guard ? PROT_NONE : (PROT_READ | PROT_WRITE),
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (map == MAP_FAILED)
        return NULL;
    uintptr_t user = (uintptr_t)map + guard;
    if (slack)
        user = (user + align - 1) & ~(uintptr_t)(align - 1);
    if (guard && mprotect((void *)user, data, PROT_READ | PROT_WRITE) != 0) {
        munmap(map, total);
        return NULL;
    }
    pthread_mutex_lock(&G.lock);
    if (G.large_live >= LARGE_SLOTS / 4 * 3) {
        pthread_mutex_unlock(&G.lock);
        munmap(map, total);
        return NULL;
    }
    large_insert(user, (uintptr_t)map, total, data);
    pthread_mutex_unlock(&G.lock);
    return (void *)user;
}

/* The block was removed from the live table: keep its mapping for reuse, or give it back. */
static void large_release(const large_ent_t *e)
{
    if (e->usable <= LCACHE_MAX_ENTRY && e->map_len == e->usable + 2 * (G.level >= 1 ? G.page : 0)) {
        large_ent_t evicted = {0, 0, 0, 0};
        pthread_mutex_lock(&G.lock);
        unsigned slot = LCACHE_SLOTS;
        for (unsigned i = 0; i < LCACHE_SLOTS; i++)
            if (!G.lcache[i].user) { slot = i; break; }
        if (slot == LCACHE_SLOTS) {                              /* full: replace the oldest */
            slot = G.lcache_next++ % LCACHE_SLOTS;
            evicted = G.lcache[slot];
            G.lcache_bytes -= evicted.map_len;
        }
        while (G.lcache_bytes + e->map_len > LCACHE_MAX_BYTES && !evicted.user) {   /* too many bytes: evict one */
            unsigned v = G.lcache_next++ % LCACHE_SLOTS;
            if (G.lcache[v].user && v != slot) {
                evicted = G.lcache[v];
                G.lcache[v].user = 0;
                G.lcache_bytes -= evicted.map_len;
            } else if (G.lcache_bytes == 0) {
                break;
            }
        }
        G.lcache[slot] = *e;
        G.lcache_bytes += e->map_len;
        pthread_mutex_unlock(&G.lock);
        if (evicted.user)
            munmap((void *)evicted.map, evicted.map_len);
        return;
    }
    munmap((void *)e->map, e->map_len);
}

/* look up (and optionally remove) a large block; returns false if p is not one */
static bool large_find(const void *p, bool remove_it, large_ent_t *out)
{
    if (!G.large)
        return false;
    uintptr_t u = (uintptr_t)p;
    pthread_mutex_lock(&G.lock);
    size_t i = large_hash(u);
    for (size_t probes = 0; probes < LARGE_SLOTS; probes++, i = (i + 1) & (LARGE_SLOTS - 1)) {
        uintptr_t k = G.large[i].user;
        if (k == LARGE_EMPTY)
            break;
        if (k == u) {
            *out = G.large[i];
            if (remove_it) {
                G.large[i].user = LARGE_TOMB;
                G.large_live--;
                G.large_bytes -= out->map_len;
            }
            pthread_mutex_unlock(&G.lock);
            return true;
        }
    }
    pthread_mutex_unlock(&G.lock);
    return false;
}

/* ---------------------------------------------------------------- public API */
static inline void ensure_init(void) { pthread_once(&once_ctl, global_init); }

void *afalloc(size_t n)
{
    if (likely(n <= SMALL_MAX)) {
        heap_t *h = get_heap();
        if (unlikely(!h)) {
            errno = ENOMEM;
            return NULL;
        }
        void *p = small_alloc(h, size_class(n));
        if (unlikely(!p))
            errno = ENOMEM;
        return p;
    }
    pthread_once(&once_ctl, global_init);
    if (G.failed) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = large_alloc(n, 0, NULL);
    if (!p)
        errno = ENOMEM;
    return p;
}

void afree(void *p)
{
    if (!p)
        return;
    uintptr_t off = (uintptr_t)p - (uintptr_t)G.base;
    if (likely(off < G.reserve)) {
        slab_meta_t *m = &G.meta[off >> SLAB_SHIFT];
        heap_t *h = tls_h;
        if (likely(h != NULL && atomic_load_explicit(&m->owner, memory_order_relaxed) == h)) {
            if (unlikely(m->flags != 0)) {
                free_checked_local(h, m, p);
                return;
            }
            block_push(h, m, p);
        } else {
            if (G.level >= 1 && m->state != 1)
                die("free of a pointer into an unused part of the heap");
            small_free_remote(m, p);
        }
        return;
    }
    ensure_init();                                  /* so the security level is known, even for a wild pointer */
    large_ent_t e;
    if (large_find(p, true, &e)) {
        large_release(&e);
        return;
    }
    if (G.level >= 1)
        die("free of a pointer that was not allocated by afalloc");
}

size_t afusable_size(const void *p)
{
    if (!p)
        return 0;
    uintptr_t off = (uintptr_t)p - (uintptr_t)G.base;
    if (off < G.reserve) {
        const slab_meta_t *m = &G.meta[off >> SLAB_SHIFT];
        return m->state == 1 ? m->block_size : 0;
    }
    ensure_init();
    large_ent_t e;
    return large_find(p, false, &e) ? e.usable : 0;
}

void *afcalloc(size_t n, size_t m)
{
    size_t total;
    if (__builtin_mul_overflow(n, m, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    if (total <= SMALL_MAX) {
        void *p = afalloc(total);
        if (p)
            memset(p, 0, total);
        return p;
    }
    pthread_once(&once_ctl, global_init);
    if (G.failed) {
        errno = ENOMEM;
        return NULL;
    }
    bool dirty = false;
    void *p = large_alloc(total, 0, &dirty);
    if (!p)
        errno = ENOMEM;
    else if (dirty)
        memset(p, 0, total);                        /* a fresh mapping is already zero; a cached one is not */
    return p;
}

void *afrealloc(void *p, size_t n)
{
    if (!p)
        return afalloc(n);
    size_t have = afusable_size(p);
    if (have == 0) {
        if (G.level >= 1)
            die("realloc of a pointer that was not allocated by afalloc");
        return NULL;
    }
    if (n <= have && (n > have / 2 || have <= 64))
        return p;
    void *q = afalloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, n < have ? n : have);
    afree(p);
    return q;
}

void *afaligned_alloc(size_t align, size_t n)
{
    if (align == 0 || (align & (align - 1))) {
        errno = EINVAL;
        return NULL;
    }
    if (align <= 16)
        return afalloc(n);
    if (n <= SMALL_MAX && align <= 4096) {
        size_t want = n > align ? n : align;
        if (want <= SMALL_MAX) {
            unsigned c = size_class(want);
            while (c < NCLASS && class_size[c] % align != 0)
                c++;
            if (c < NCLASS) {
                heap_t *h = get_heap();
                if (!h) {
                    errno = ENOMEM;
                    return NULL;
                }
                void *p = small_alloc(h, c);
                if (!p)
                    errno = ENOMEM;
                return p;
            }
        }
    }
    pthread_once(&once_ctl, global_init);
    if (G.failed) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = large_alloc(n, align, NULL);
    if (!p)
        errno = ENOMEM;
    return p;
}

int afcheck(void)
{
    if (!atomic_load(&G.ready))
        return 0;
    int rc = 0;
    pthread_mutex_lock(&G.lock);
    if (G.slabs_in_use + G.nfree != G.next_fresh)
        rc = 1;                                         /* a slab was lost or counted twice */
    for (unsigned c = 0; c < NCLASS && !rc; c++) {
        slab_meta_t *prev = NULL;
        for (slab_meta_t *s = G.abandoned[c]; s; prev = s, s = s->ab_next) {
            if (!s->ab_listed || s->state != 1 || s->cls != c || atomic_load(&s->owner) != NULL || s->ab_prev != prev ||
                s->ab_live == 0) {
                rc = 2;                                 /* a damaged abandoned list */
                break;
            }
        }
    }
    for (uint32_t i = 0; i < G.nfree && !rc; i++)
        if (G.meta[G.freestack[i]].state != 0)
            rc = 3;                                     /* a slab in the free pool is still marked in use */
    pthread_mutex_unlock(&G.lock);
    return rc;
}

void afget_stats(afstats_t *out)
{
    memset(out, 0, sizeof *out);
    if (!atomic_load(&G.ready))
        return;
    pthread_mutex_lock(&G.lock);
    out->heap_reserved = G.reserve;
    out->slabs_in_use = G.slabs_in_use;
    out->slabs_committed = G.next_fresh;
    out->large_live = G.large_live;
    out->large_bytes = G.large_bytes;
    out->security_level = G.level;
    for (unsigned c = 0; c < NCLASS; c++)
        for (slab_meta_t *a = G.abandoned[c]; a; a = a->ab_next)
            out->slabs_abandoned++;
    pthread_mutex_unlock(&G.lock);
}

/* ---------------------------------------------------------------- thread exit */
static void af_thread_exit_(void *arg)
{
    heap_t *h = arg;
    if (!h)
        return;
    tls_h = NULL;
    drain_pending(h);
    for (unsigned i = 0; i < QUARANTINE; i++) {         /* free the quarantined blocks for real */
        void *q = h->quarantine[i];
        if (!q)
            continue;
        h->quarantine[i] = NULL;
        slab_meta_t *m = &G.meta[((uintptr_t)q - (uintptr_t)G.base) >> SLAB_SHIFT];
        if (atomic_load_explicit(&m->owner, memory_order_relaxed) == h)
            block_push(h, m, q);
        else
            small_free_remote(m, q);
    }
    slab_meta_t *s = h->all, *next;
    for (; s; s = next) {
        next = s->all_next;
        if (h->cur[s->cls] != s && atomic_load_explicit(&s->linked, memory_order_relaxed))
            avail_remove(h, s);
        all_remove(h, s);
        atomic_store_explicit(&s->linked, 0, memory_order_relaxed);
        pthread_mutex_lock(&G.lock);
        collect_remote(s);                          /* under the lock, so the count below is exact */
        if (s->used == 0) {
            atomic_store_explicit(&s->owner, NULL, memory_order_relaxed);
            slab_return_locked(s);
        } else {
            s->ab_live = s->used;
            atomic_store_explicit(&s->owner, NULL, memory_order_release);
            ab_push(s);
        }
        pthread_mutex_unlock(&G.lock);
    }
    pthread_mutex_lock(&G.lock);
    h->next_free = G.heap_free;
    G.heap_free = h;
    pthread_mutex_unlock(&G.lock);
}
