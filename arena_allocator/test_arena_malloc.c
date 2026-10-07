#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "arena_malloc.h"

#ifndef AFA_SIZE
#define AFA_SIZE ((size_t)1024 * 1024)
#endif
#define ARENA ((size_t)AFA_SIZE)
#define OVERHEAD (4 * sizeof(size_t))   /* header + footer */

static void test_bounds(void){
    assert(afalloc(0) == NULL);
    assert(afalloc(ARENA) == NULL);
    assert(afalloc(SIZE_MAX) == NULL);

    /* regression: the capacity check used to run before aligning, so a
       request near the end of the arena overran it by up to 7 bytes */
    size_t max = ARENA - OVERHEAD;
    assert(afalloc(max - 7) != NULL);     /* aligns up to exactly max: fits */
    assert(afalloc(1) == NULL);
    afa_reset();
    assert(afalloc(max + 1) == NULL);     /* aligns past the end: rejected */
    unsigned char *p = afalloc(max);
    assert(p != NULL);
    memset(p, 0xCD, max);                 /* ASan flags any overrun */
    afa_reset();
    unsigned char *q = afalloc(ARENA - OVERHEAD - 8 - OVERHEAD - 3);
    assert(q != NULL);
    assert(afalloc(8) != NULL);
    afa_reset();
}

static void test_free_and_coalesce(void){
    unsigned char *a = afalloc(32);
    unsigned char *b = afalloc(32);
    unsigned char *c = afalloc(32);
    unsigned char *d = afalloc(32);
    assert(a && b && c && d);
    assert((uintptr_t)a % 8 == 0);

    arena_free(NULL);
    arena_free(a);
    arena_free(a);                        /* double free is ignored */
    arena_free(c);
    arena_free(b);                        /* merges backward into a and forward into c */
    memset(a, 0xEE, 32 + OVERHEAD + 32 + OVERHEAD + 32);   /* a..c is one 128+ byte block */
    memset(d, 0x11, 32);
    for (int i = 0; i < 32; i++) assert(d[i] == 0x11);
    afa_reset();
}

static void test_reuse(void){
    /* a freed block is handed out again, split when much larger than needed */
    unsigned char *a = afalloc(256);
    unsigned char *b = afalloc(64);
    assert(a && b);
    arena_free(a);
    unsigned char *c = afalloc(64);
    assert(c == a);                       /* first fit lands in the freed block */
    unsigned char *d = afalloc(64);       /* the split remainder is reusable too */
    assert(d && d > c && d < b);
    afa_reset();

    /* freeing the last block returns it to the frontier */
    unsigned char *x = afalloc(100);
    unsigned char *y = afalloc(100);
    assert(x && y);
    arena_free(y);
    assert(afalloc(100) == y);
    arena_free(y);
    arena_free(x);                        /* merges backward, then reaches the frontier */
    assert(afalloc(100) == x);
    afa_reset();

    /* the whole arena can be filled, freed and filled again */
    size_t max = ARENA - OVERHEAD;
    unsigned char *big = afalloc(max);
    assert(big != NULL && afalloc(1) == NULL);
    arena_free(big);
    assert(afalloc(max) == big);
    afa_reset();
}

int main(void){
    test_bounds();
    test_reuse();
    test_free_and_coalesce();
    puts("test_arena_malloc: all tests passed");
    return 0;
}
