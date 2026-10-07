#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "afalloc_persistent.h"

#define CHUNK ((size_t)1024 * 1024)
#define HEADER (2 * sizeof(size_t))
#define MAX_REQ (CHUNK - HEADER)
#define POOL_CHUNKS 10

static void test_basic(void){
    assert(afalloc(0) == NULL);
    assert(afalloc_persistent(0) == NULL);

    unsigned char *a = afalloc(1);
    unsigned char *b = afalloc(1);
    assert(a && b);
    assert((uintptr_t)a % 8 == 0 && (uintptr_t)b % 8 == 0);
    assert(b == a + 8 + HEADER);   /* 1 byte rounds up to 8, plus one header */
    afa_reset();
}

static void test_oversized_does_not_poison(void){
    /* regression: a request larger than a chunk used to walk and mmap every
       remaining chunk, then leave the allocator permanently exhausted */
    assert(afalloc(2 * CHUNK) == NULL);
    assert(afalloc(CHUNK) == NULL);
    assert(afalloc(MAX_REQ + 1) == NULL);
    assert(afalloc(SIZE_MAX) == NULL);
    assert(afalloc(SIZE_MAX - 3) == NULL);
    assert(afalloc_persistent(2 * CHUNK) == NULL);
    assert(afalloc_persistent(SIZE_MAX) == NULL);

    void *p = afalloc(64);
    assert(p != NULL);
    afa_reset();
}

static void test_max_request(void){
    unsigned char *p = afalloc(MAX_REQ);
    assert(p != NULL);
    memset(p, 0xAB, MAX_REQ);          /* ASan flags this if the block overruns */
    assert(afalloc(1) != NULL);        /* spills into the next chunk */
    afa_reset();
}

static void test_chunk_chaining_and_exhaustion(void){
    /* each pool holds POOL_CHUNKS chunks; one MAX_REQ block fills a chunk */
    for (int i = 0; i < POOL_CHUNKS; i++) assert(afalloc(MAX_REQ) != NULL);
    assert(afalloc(8) == NULL);
    afa_reset();
    assert(afalloc(MAX_REQ) != NULL);  /* chunks are reused after reset */
    afa_reset();
}

static void test_reset_reuses_memory(void){
    void *a = afalloc(100);
    afa_reset();
    void *b = afalloc(100);
    assert(a == b);
    afa_reset();
}

static void test_persistent_survives_reset(void){
    char *keep = afalloc_persistent(32);
    assert(keep != NULL);
    memset(keep, 0x5A, 32);
    void *s = afalloc(32);
    assert(s != NULL && s != (void*)keep);
    afa_reset();
    for (int i = 0; i < 32; i++) assert(keep[i] == 0x5A);
    char *keep2 = afalloc_persistent(32);
    assert(keep2 != NULL && keep2 != keep);
    for (int i = 0; i < 32; i++) assert(keep[i] == 0x5A);
}

static void test_persistent_chaining(void){
    /* the persistent pool chains across chunks like the scratch pool */
    void *blocks[POOL_CHUNKS + 1];
    int got = 0;
    for (int i = 0; i < POOL_CHUNKS + 1; i++) {
        blocks[i] = afalloc_persistent(MAX_REQ);
        if (blocks[i]) got++;
    }
    /* chunk 0 already holds the small blocks from the previous test, so one
       MAX_REQ block does not fit there; later chunks each fit exactly one */
    assert(got >= POOL_CHUNKS - 1 && got <= POOL_CHUNKS);
    assert(blocks[POOL_CHUNKS] == NULL);
    memset(blocks[got - 1], 0x11, MAX_REQ);   /* last block really is mapped */
    afa_destroy();                            /* clean slate for later tests */
}

static void test_free_lists(void){
    void *first = afalloc(8);
    assert(first != NULL);
    afree(NULL);                              /* no-op */

    void *a = afalloc(100);
    void *b = afalloc(100);
    void *c = afalloc(4000);
    assert(a && b && c);

    afree(a);
    void *a2 = afalloc(100);
    assert(a2 == a);                          /* same class: block reused, O(1) */

    afree(b);
    afree(b);                                 /* double free is ignored */
    void *b2 = afalloc(100);
    void *b3 = afalloc(100);
    assert(b2 == b);
    assert(b3 != b && b3 != a2);              /* b was handed out only once */

    afree(c);
    void *d = afalloc(100);
    assert(d != c);                           /* different class is not reused */
    void *c2 = afalloc(4000);
    assert(c2 == c);

    /* classes round up: 257..320 share one class */
    void *e = afalloc(257);
    afree(e);
    assert(afalloc(320) == e);
    assert(afalloc(321) != e);

    /* a block freed to the persistent pool stays in the persistent pool */
    void *p = afalloc_persistent(48);
    afree(p);
    assert(afalloc(48) != p);
    assert(afalloc_persistent(48) == p);

    afa_reset();
    assert(afalloc(100) == first);            /* reset clears the free lists: bump from the base */
    afa_destroy();
}

static void test_trim_and_destroy(void){
    char *keep = afalloc_persistent(16);
    assert(keep != NULL);
    memset(keep, 0x77, 16);
    for (int i = 0; i < 5; i++) assert(afalloc(MAX_REQ) != NULL);

    afa_trim();                               /* scratch chunks go back to the OS */
    for (int i = 0; i < 16; i++) assert(keep[i] == 0x77);   /* persistent untouched */
    for (int i = 0; i < POOL_CHUNKS; i++) assert(afalloc(MAX_REQ) != NULL);   /* remaps lazily */
    assert(afalloc(8) == NULL);

    afa_destroy();
    char *again = afalloc_persistent(16);
    assert(again != NULL);
    memset(again, 1, 16);
    assert(afalloc(MAX_REQ) != NULL);
    afa_destroy();
}

int main(void){
    test_basic();
    test_oversized_does_not_poison();
    test_max_request();
    test_chunk_chaining_and_exhaustion();
    test_reset_reuses_memory();
    test_persistent_survives_reset();
    test_persistent_chaining();
    test_free_lists();
    test_trim_and_destroy();
    puts("test_persistent: all tests passed");
    return 0;
}
