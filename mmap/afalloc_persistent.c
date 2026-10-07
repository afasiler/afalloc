#include <stddef.h>
#include <sys/mman.h>
#include "afalloc_persistent.h"

#ifndef AFA_NO_LOCK
#include <sched.h>
#include <stdatomic.h>
static atomic_flag afa_lock = ATOMIC_FLAG_INIT;
static void lock_acquire(void){
    while (atomic_flag_test_and_set_explicit(&afa_lock, memory_order_acquire))
        sched_yield();
}
static void lock_release(void){
    atomic_flag_clear_explicit(&afa_lock, memory_order_release);
}
#else
#define lock_acquire() ((void)0)
#define lock_release() ((void)0)
#endif

/* Chunk/region size in bytes; build with -DAFA_SIZE=<bytes> to change it. */
#ifndef AFA_SIZE
#define AFA_SIZE ((size_t)1024 * 1024)
#endif
#define MEM_SIZE ((size_t)AFA_SIZE)
#define MAX_CHUNKS 10

#define STATE_FREE       ((size_t)0xAFA2F4EE)
#define STATE_SCRATCH    ((size_t)0xAFA25C7A)
#define STATE_PERSISTENT ((size_t)0xAFA29E55)

/* Two size_t fields keep the header a multiple of 8 bytes on both 32-bit and
   64-bit targets, so user pointers stay 8-byte aligned. */
struct metadata{
    size_t size;
    size_t state;
};

#define HEADER_SIZE sizeof(struct metadata)
#define MAX_REQUEST (MEM_SIZE - HEADER_SIZE)

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

typedef struct {
    unsigned char *memory;
    size_t frontier;
} chunk_t;

typedef struct {
    chunk_t chunks[MAX_CHUNKS];
    int current;
    struct free_block *free_lists[NUM_CLASSES];
    int max_class;             /* highest class index that ever held a free block */
    size_t used_state;
} pool_t;

static pool_t scratch_pool    = { .max_class = -1, .used_state = STATE_SCRATCH };
static pool_t persistent_pool = { .max_class = -1, .used_state = STATE_PERSISTENT };

/* Maps a request to its class index and the exact block size of that class.
   Requests above MAX_REQUEST must be rejected by the caller first. */
static int size_class(size_t size, size_t *rounded){
    if (size <= SMALL_LIMIT) {
        *rounded = (size + 7) & ~(size_t)7;
        return (int)(*rounded / 8) - 1;
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

static int map_chunk(chunk_t *c){
    unsigned char *base = (unsigned char*)mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
                                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return -1;
    c->memory = base;
    c->frontier = 0;
    return 0;
}

static void *bump(chunk_t *c, size_t state, size_t size){
    if (MEM_SIZE - c->frontier < HEADER_SIZE) return NULL;
    if (size > MEM_SIZE - c->frontier - HEADER_SIZE) return NULL;
    struct metadata *header = (struct metadata *)&c->memory[c->frontier];
    header->size = size;
    header->state = state;
    c->frontier += HEADER_SIZE + size;
    return (void*)(header + 1);
}

static void *pool_alloc(pool_t *pool, size_t size){
    if (size == 0 || size > MAX_REQUEST) return NULL;   /* never fits: no state touched */
    size_t rounded;
    int idx = size_class(size, &rounded);

    struct free_block *fb = pool->free_lists[idx];
    if (fb) {
        pool->free_lists[idx] = fb->next;
        ((struct metadata *)fb - 1)->state = pool->used_state;
        return fb;
    }

    while (pool->current < MAX_CHUNKS) {
        chunk_t *c = &pool->chunks[pool->current];
        if (c->memory == NULL && map_chunk(c) != 0) return NULL;
        void *p = bump(c, pool->used_state, rounded);
        if (p) return p;
        pool->current++;
    }
    return NULL;
}

static void pool_reset(pool_t *pool){
    for (int i = 0; i < MAX_CHUNKS; i++) pool->chunks[i].frontier = 0;
    for (int i = 0; i <= pool->max_class; i++) pool->free_lists[i] = NULL;
    pool->max_class = -1;
    pool->current = 0;
}

static void pool_release(pool_t *pool){
    for (int i = 0; i < MAX_CHUNKS; i++) {
        if (pool->chunks[i].memory != NULL) {
            munmap(pool->chunks[i].memory, MEM_SIZE);
            pool->chunks[i].memory = NULL;
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
    struct metadata *h = (struct metadata *)ptr - 1;
    pool_t *pool = NULL;
    if (h->state == STATE_SCRATCH) pool = &scratch_pool;
    else if (h->state == STATE_PERSISTENT) pool = &persistent_pool;
    if (pool != NULL) {                    /* STATE_FREE / unknown: ignored */
        size_t rounded;
        int idx = size_class(h->size, &rounded);
        struct free_block *fb = (struct free_block *)ptr;
        fb->next = pool->free_lists[idx];
        pool->free_lists[idx] = fb;
        if (idx > pool->max_class) pool->max_class = idx;
        h->state = STATE_FREE;
    }
    lock_release();
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
