#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "mmap_allocator.h"

/* randomized alloc/free/reset against a shadow model of the live blocks:
   every block must stay 8-aligned, disjoint and keep its pattern */

#define MAX_LIVE 2048
#define ROUNDS 400000

struct live { unsigned char *p; size_t size; unsigned char tag; };
static struct live blocks[MAX_LIVE];
static size_t n_live;

static uint64_t rng = 0x1234567890ABCDEFull;
static uint64_t next(void){
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static void fill(const struct live *b){
    for (size_t i = 0; i < b->size; i++) b->p[i] = (unsigned char)(b->tag + i);
}
static void check(const struct live *b){
    for (size_t i = 0; i < b->size; i++)
        assert(b->p[i] == (unsigned char)(b->tag + i));
}

static int cmp(const void *a, const void *b){
    const struct live *x = a, *y = b;
    return (x->p > y->p) - (x->p < y->p);
}
static void check_all(void){
    static struct live sorted[MAX_LIVE];
    for (size_t i = 0; i < n_live; i++) { check(&blocks[i]); sorted[i] = blocks[i]; }
    qsort(sorted, n_live, sizeof sorted[0], cmp);
    for (size_t i = 0; i < n_live; i++) {
        assert((uintptr_t)sorted[i].p % 8 == 0);
        if (i + 1 < n_live) assert(sorted[i].p + sorted[i].size <= sorted[i + 1].p);
    }
}

static size_t pick_size(void){
    uint64_t r = next() % 100;
    if (r < 70) return 1 + next() % 128;
    if (r < 97) return 1 + next() % 4096;
    return 1 + next() % (64 * 1024);
}

int main(void){
    size_t nulls = 0, resets = 0, frees = 0, allocs = 0;
    for (int round = 0; round < ROUNDS; round++) {
        uint64_t op = next() % 1000;
        if (op < 3) {
            check_all();
            reset_region();
            n_live = 0;
            resets++;
            struct live probe = { afalloc(64), 64, 7 };   /* region is empty again */
            assert(probe.p != NULL);
            f_free(probe.p);
        } else if (op < 480 && n_live > 0) {
            size_t k = next() % n_live;
            check(&blocks[k]);
            f_free(blocks[k].p);
            blocks[k] = blocks[--n_live];
            frees++;
        } else if (n_live < MAX_LIVE) {
            size_t sz = pick_size();
            unsigned char *p = afalloc(sz);
            if (!p) { nulls++; continue; }
            blocks[n_live] = (struct live){p, sz, (unsigned char)next()};
            fill(&blocks[n_live++]);
            allocs++;
        }
        if (round % 5000 == 0) check_all();
    }
    check_all();
    /* deterministic seed: the live set stays well under 1 MiB, so a NULL here
       means freed blocks were not found again (a lost-free-block bug) */
    assert(nulls == 0);
    printf("stress_gp: %zu allocs, %zu frees, %zu resets, %zu NULL returns, all patterns intact\n",
           allocs, frees, resets, nulls);
    return 0;
}
