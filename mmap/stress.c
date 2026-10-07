#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "afalloc_persistent.h"

#define MAX_LIVE 4096
#define ROUNDS 200000

struct live { unsigned char *p; size_t size; unsigned char tag; };

static struct live scratch[MAX_LIVE];
static struct live persist[MAX_LIVE];
static size_t n_scratch, n_persist;

static uint64_t rng = 88172645463325252ULL;
static uint64_t next(void){
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static size_t pick_size(void){
    uint64_t r = next() % 100;
    if (r < 70) return 1 + next() % 128;
    if (r < 95) return 1 + next() % 8192;
    return 1 + next() % (200 * 1024);
}

static void fill(struct live *b){
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

/* every live block must be 8-aligned and no two may overlap */
static void check_no_overlap(const struct live *a, size_t n){
    static struct live sorted[MAX_LIVE];
    for (size_t i = 0; i < n; i++) sorted[i] = a[i];
    qsort(sorted, n, sizeof sorted[0], cmp);
    for (size_t i = 0; i < n; i++) {
        assert((uintptr_t)sorted[i].p % 8 == 0);
        if (i + 1 < n) assert(sorted[i].p + sorted[i].size <= sorted[i + 1].p);
    }
}

static void check_all(void){
    for (size_t i = 0; i < n_scratch; i++) check(&scratch[i]);
    for (size_t i = 0; i < n_persist; i++) check(&persist[i]);
}

int main(void){
    size_t resets = 0, nulls = 0;
    for (int round = 0; round < ROUNDS; round++) {
        uint64_t op = next() % 1000;
        if (op < 2) {
            check_all();
            check_no_overlap(scratch, n_scratch);
            check_no_overlap(persist, n_persist);
            if (op == 1) afa_trim(); else afa_reset();
            n_scratch = 0;
            resets++;
        } else if (op < 10) {
            /* free a random live block (checks its pattern first) */
            if (n_scratch > 0) {
                size_t i = next() % n_scratch;
                check(&scratch[i]);
                afree(scratch[i].p);
                scratch[i] = scratch[--n_scratch];
            }
        } else if (op < 14) {
            if (n_persist > 0) {
                size_t i = next() % n_persist;
                check(&persist[i]);
                afree(persist[i].p);
                persist[i] = persist[--n_persist];
            }
        } else if (op < 30) {
            if (n_persist == MAX_LIVE) continue;
            size_t sz = 1 + next() % 512;
            unsigned char *p = afalloc_persistent(sz);
            if (!p) { nulls++; continue; }
            persist[n_persist++] = (struct live){p, sz, (unsigned char)next()};
            fill(&persist[n_persist - 1]);
        } else {
            if (n_scratch == MAX_LIVE) { afa_reset(); n_scratch = 0; resets++; continue; }
            size_t sz = pick_size();
            unsigned char *p = afalloc(sz);
            if (!p) {
                /* exhausted: everything must still be intact, then reset */
                nulls++;
                check_all();
                afa_reset();
                n_scratch = 0;
                resets++;
                continue;
            }
            scratch[n_scratch++] = (struct live){p, sz, (unsigned char)next()};
            fill(&scratch[n_scratch - 1]);
        }
    }
    check_all();
    printf("stress: %d ops, %zu resets, %zu NULL returns, all patterns intact\n",
           ROUNDS, resets, nulls);
    return 0;
}
