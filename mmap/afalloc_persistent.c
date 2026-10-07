#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include "afalloc_persistent.h"

#ifndef AFA_NO_LOCK
#include <sched.h>
#include <stdatomic.h>
/* Own cache line: the lock sits next to hot allocator state otherwise, and an
   atomic on a line with pending stores is far slower than on an idle line. */
static _Alignas(64) atomic_flag afa_lock = ATOMIC_FLAG_INIT;
static inline void lock_acquire(void){
    while (atomic_flag_test_and_set_explicit(&afa_lock, memory_order_acquire))
        sched_yield();
}
static inline void lock_release(void){
    atomic_flag_clear_explicit(&afa_lock, memory_order_release);
}
#else
#define lock_acquire() ((void)0)
#define lock_release() ((void)0)
#endif

#if defined(__GNUC__)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif

/* Chunk size in bytes; build with -DAFA_SIZE=<bytes> to change it. */
#ifndef AFA_SIZE
#define AFA_SIZE ((size_t)1024 * 1024)
#endif
#define MEM_SIZE ((size_t)AFA_SIZE)
#define MAX_CHUNKS 10

/* Every block starts with one 8-byte header word (AFA_HEADER_SIZE), so user
   pointers are 8-byte aligned on 32- and 64-bit targets alike:

     bits 63..32  magic, rejects pointers that did not come from here
     bits 31..24  unused
     bits 23..8   size class (see size_class()); the class, not the size, is
                  all afree() needs, so it is never recomputed
     bits  7..0   state: scratch, persistent or free                         */
#define HEADER_SIZE ((size_t)AFA_HEADER_SIZE)
#define HDR_MAGIC   ((uint64_t)0xAFA2C0DE << 32)
#define HDR_MASK    ((uint64_t)0xFFFFFFFF << 32)
#define STATE_FREE       ((uint64_t)0xF4)
#define STATE_SCRATCH    ((uint64_t)0x5C)
#define STATE_PERSISTENT ((uint64_t)0x9E)
#define MAX_REQUEST (MEM_SIZE - HEADER_SIZE)

static inline uint64_t make_header(uint64_t state, int cls){
    return HDR_MAGIC | ((uint64_t)cls << 8) | state;
}

/* Size classes: every multiple of 8 up to 256 (32 classes), then 4 classes per
   power of two (at most 25% internal waste) up to one chunk. */
#define SMALL_LIMIT 256
#define SMALL_CLASSES (SMALL_LIMIT / 8)
/* enough classes for any chunk size: class indices stay below
   SMALL_CLASSES + 4 * (bits in size_t - 8) */
#define NUM_CLASSES (SMALL_CLASSES + 4 * ((int)(sizeof(size_t) * 8) - 8))

struct free_block{
    struct free_block *next;   /* lives in the user area of a freed block */
};

/* Bump window first so the fast path touches one cache line. Chunks are filled
   strictly in order, so only the current chunk needs a frontier. */
typedef struct {
    unsigned char *cur;        /* next free byte of the current chunk */
    size_t left;               /* bytes remaining in the current chunk */
    int current;               /* index of the current chunk, -1 before the first */
    int max_class;             /* highest class index that ever held a free block */
    uint64_t used_header;      /* header template for blocks handed out */
    unsigned char *chunks[MAX_CHUNKS];
    struct free_block *free_lists[NUM_CLASSES];
} pool_t;

static _Alignas(64) pool_t scratch_pool = {
    .current = -1, .max_class = -1, .used_header = HDR_MAGIC | STATE_SCRATCH };
static _Alignas(64) pool_t persistent_pool = {
    .current = -1, .max_class = -1, .used_header = HDR_MAGIC | STATE_PERSISTENT };

/* Maps a request to its class index and the exact block size of that class.
   Requests above MAX_REQUEST must be rejected by the caller first. */
static inline int size_class(size_t size, size_t *rounded){
    if (LIKELY(size <= SMALL_LIMIT)) {
        *rounded = (size + 7) & ~(size_t)7;
        return (int)(*rounded >> 3) - 1;
    }
    size_t v = size - 1;
#if defined(__GNUC__)
    int k = (int)(sizeof(unsigned long) * 8 - 1 - (unsigned)__builtin_clzl(v));
#else
    int k = 0;
    while (v >> (k + 1)) k++;
#endif
    int shift = k - 2;
    size_t step = v >> shift;                  /* 4..7 */
    *rounded = (step + 1) << shift;
    if (*rounded > MAX_REQUEST) *rounded = MAX_REQUEST;
    return SMALL_CLASSES + (k - 8) * 4 + (int)(step - 4);
}

/* Block size of a class (the inverse of size_class()). */
static size_t class_size(int cls){
    if (cls < SMALL_CLASSES) return (size_t)(cls + 1) * 8;
    int k = (cls - SMALL_CLASSES) / 4 + 8;
    size_t step = (size_t)((cls - SMALL_CLASSES) % 4) + 4;
    size_t r = (step + 1) << (k - 2);
    return r > MAX_REQUEST ? MAX_REQUEST : r;
}

/* populate != 0 faults every page in now (MAP_POPULATE on Linux, one write per
   page elsewhere) so first use of the chunk never takes a page fault. Building
   with -DAFA_PREFAULT does that for every chunk as it is mapped. */
static unsigned char *map_chunk(int populate){
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(AFA_PREFAULT)
    populate = 1;
#endif
#if defined(MAP_POPULATE)
    if (populate) flags |= MAP_POPULATE;
#endif
    void *base = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (base == MAP_FAILED) return NULL;
#if !defined(MAP_POPULATE)
    if (populate)
        for (size_t off = 0; off < MEM_SIZE; off += 4096)
            ((volatile unsigned char *)base)[off] = 0;
#endif
    return (unsigned char *)base;
}

static void push_free(pool_t *pool, void *user, int cls){
    struct free_block *fb = (struct free_block *)user;
    ((uint64_t *)user)[-1] = make_header(STATE_FREE, cls);
    fb->next = pool->free_lists[cls];
    pool->free_lists[cls] = fb;
    if (cls > pool->max_class) pool->max_class = cls;
}

/* The rest of the old chunk is turned into free blocks instead of being lost
   until the next reset: repeatedly carve the largest class that still fits. */
static void recycle_tail(pool_t *pool){
    while (pool->left >= HEADER_SIZE + 8) {
        size_t usable = pool->left - HEADER_SIZE;
        size_t rounded;
        int cls = size_class(usable > MAX_REQUEST ? MAX_REQUEST : usable, &rounded);
        if (rounded > usable) {                 /* class one step down always fits */
            cls--;
            rounded = class_size(cls);
        }
        push_free(pool, pool->cur + HEADER_SIZE, cls);
        pool->cur += HEADER_SIZE + rounded;
        pool->left -= HEADER_SIZE + rounded;
    }
}

/* Slow path: the current chunk cannot hold `need` bytes. Move on to the next
   chunk (mapping it if necessary). The window only changes once the new chunk
   exists, so a failed mmap leaves the old tail usable for smaller requests. */
static int pool_next_chunk(pool_t *pool){
    for (int i = pool->current + 1; i < MAX_CHUNKS; i++) {
        if (pool->chunks[i] == NULL) {
            pool->chunks[i] = map_chunk(0);
            if (pool->chunks[i] == NULL) return -1;
        }
        if (pool->cur != NULL) recycle_tail(pool);
        pool->current = i;
        pool->cur = pool->chunks[i];
        pool->left = MEM_SIZE;
        return 0;
    }
    return -1;
}

static void *pool_alloc(pool_t *pool, size_t size){
    if (UNLIKELY(size == 0 || size > MAX_REQUEST)) return NULL;   /* never fits: no state touched */
    size_t rounded;
    int idx = size_class(size, &rounded);

    struct free_block *fb = pool->free_lists[idx];
    if (fb) {
        pool->free_lists[idx] = fb->next;
        ((uint64_t *)fb)[-1] = pool->used_header | ((uint64_t)idx << 8);
        return fb;
    }

    size_t need = HEADER_SIZE + rounded;
    if (UNLIKELY(pool->left < need)) {
        if (pool_next_chunk(pool) != 0) return NULL;
    }
    unsigned char *block = pool->cur;
    pool->cur += need;
    pool->left -= need;
    *(uint64_t *)block = pool->used_header | ((uint64_t)idx << 8);
    return block + HEADER_SIZE;
}

static void pool_reset(pool_t *pool){
    for (int i = 0; i <= pool->max_class; i++) pool->free_lists[i] = NULL;
    pool->max_class = -1;
    if (pool->chunks[0] != NULL) {
        pool->current = 0;
        pool->cur = pool->chunks[0];
        pool->left = MEM_SIZE;
    } else {
        pool->current = -1;
        pool->cur = NULL;
        pool->left = 0;
    }
}

static void pool_release(pool_t *pool){
    for (int i = 0; i < MAX_CHUNKS; i++) {
        if (pool->chunks[i] != NULL) {
            munmap(pool->chunks[i], MEM_SIZE);
            pool->chunks[i] = NULL;
        }
    }
    pool_reset(pool);
}

void *afalloc_persistent(size_t size){
    lock_acquire();
    void *p = pool_alloc(&persistent_pool, size);
    lock_release();
    return p;
}

void *afalloc(size_t size){
    lock_acquire();
    void *p = pool_alloc(&scratch_pool, size);
    lock_release();
    return p;
}

void afree(void *ptr){
    if (ptr == NULL) return;
    lock_acquire();
    uint64_t h = ((uint64_t *)ptr)[-1];
    pool_t *pool = NULL;
    if ((h & HDR_MASK) == HDR_MAGIC) {
        uint64_t state = h & 0xFF;
        if (state == STATE_SCRATCH) pool = &scratch_pool;
        else if (state == STATE_PERSISTENT) pool = &persistent_pool;
    }
    int cls = (int)((h >> 8) & 0xFFFF);
    if (pool != NULL && cls < NUM_CLASSES) {   /* STATE_FREE / unknown: ignored */
        push_free(pool, ptr, cls);
    }
    lock_release();
}

int afa_prefault(unsigned scratch_chunks, unsigned persistent_chunks){
    int rc = 0;
    lock_acquire();
    for (unsigned pass = 0; pass < 2 && rc == 0; pass++) {
        pool_t *pool = pass ? &persistent_pool : &scratch_pool;
        unsigned n = pass ? persistent_chunks : scratch_chunks;
        if (n > MAX_CHUNKS) n = MAX_CHUNKS;
        for (unsigned i = 0; i < n; i++) {
            if (pool->chunks[i] != NULL) continue;
            pool->chunks[i] = map_chunk(1);
            if (pool->chunks[i] == NULL) { rc = -1; break; }
        }
    }
    lock_release();
    return rc;
}

void afa_reset(void){
    lock_acquire();
    pool_reset(&scratch_pool);
    lock_release();
}

void afa_trim(void){
    lock_acquire();
    pool_release(&scratch_pool);
    lock_release();
}

void afa_destroy(void){
    lock_acquire();
    pool_release(&scratch_pool);
    pool_release(&persistent_pool);
    lock_release();
}
