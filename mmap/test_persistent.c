#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "afalloc_persistent.h"

#define CHUNK ((size_t)1024 * 1024)
#define HEADER (2 * sizeof(size_t))
#define MAX_REQ (CHUNK - HEADER)

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
    /* 9 scratch chunks (chunk 0 is persistent); each holds exactly one
       MAX_REQ block */
    for (int i = 0; i < 9; i++) assert(afalloc(MAX_REQ) != NULL);
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
    assert(afalloc_persistent(MAX_REQ) == NULL);  /* persistent chunk is partly used */
}

int main(void){
    test_basic();
    test_oversized_does_not_poison();
    test_max_request();
    test_chunk_chaining_and_exhaustion();
    test_reset_reuses_memory();
    test_persistent_survives_reset();
    puts("test_persistent: all tests passed");
    return 0;
}
