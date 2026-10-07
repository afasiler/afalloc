/* Compares glibc malloc/free with afalloc/afree on two workloads:
     bulk  - allocate many small blocks, then release them all at once
     churn - random alloc/free over a fixed live set, timing every call
   Build with optimisation and no sanitizers (make bench). */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "afalloc_persistent.h"

#define BULK_N 20000
#define BULK_ROUNDS 200
#define SLOTS 2048
#define CHURN_OPS 2000000

static uint64_t rng = 88172645463325252ULL;
static uint64_t next(void){
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static uint64_t now_ns(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void *glibc_alloc(size_t n){ return malloc(n); }
static void  glibc_free(void *p){ free(p); }
static void *af_alloc(size_t n){ return afalloc(n); }
static void  af_free(void *p){ afree(p); }

static int cmp_u32(const void *a, const void *b){
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

static void bulk(const char *name, void *(*al)(size_t), void (*fr)(void*), int use_reset){
    static void *ptrs[BULK_N];
    uint64_t t0 = now_ns();
    for (int r = 0; r < BULK_ROUNDS; r++) {
        for (int i = 0; i < BULK_N; i++) {
            ptrs[i] = al(16 + (size_t)(next() % 241));
            if (!ptrs[i]) { fprintf(stderr, "%s: out of memory\n", name); exit(1); }
            *(volatile char*)ptrs[i] = 1;
        }
        if (use_reset) afa_reset();
        else for (int i = 0; i < BULK_N; i++) fr(ptrs[i]);
    }
    uint64_t dt = now_ns() - t0;
    double ops = (double)BULK_N * BULK_ROUNDS;
    printf("  %-28s %8.1f ns per alloc (incl. release)\n", name, (double)dt / ops);
}

static void churn(const char *name, void *(*al)(size_t), void (*fr)(void*)){
    static void *slot[SLOTS];
    static uint32_t lat[CHURN_OPS];
    memset(slot, 0, sizeof slot);
    size_t n = 0;
    for (int op = 0; op < CHURN_OPS; op++) {
        size_t i = (size_t)(next() % SLOTS);
        uint64_t t0 = now_ns();
        if (slot[i]) { fr(slot[i]); slot[i] = NULL; }
        else {
            slot[i] = al(16 + (size_t)(next() % 1009));
            if (!slot[i]) { fprintf(stderr, "%s: out of memory\n", name); exit(1); }
        }
        uint64_t dt = now_ns() - t0;
        lat[n++] = dt > UINT32_MAX ? UINT32_MAX : (uint32_t)dt;
    }
    for (int i = 0; i < SLOTS; i++) if (slot[i]) fr(slot[i]);
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += lat[i];
    qsort(lat, n, sizeof lat[0], cmp_u32);
    printf("  %-28s mean %6.1f  p50 %5u  p99 %5u  p99.99 %6u  max %8u  (ns/op)\n",
           name, (double)sum / (double)n, lat[n / 2], lat[n * 99 / 100],
           lat[n * 9999 / 10000], lat[n - 1]);
}

int main(void){
    printf("bulk: %d small allocs, then release all, x%d\n", BULK_N, BULK_ROUNDS);
    bulk("glibc malloc + free each", glibc_alloc, glibc_free, 0);
    bulk("afalloc + afree each", af_alloc, af_free, 0);
    bulk("afalloc + afa_reset", af_alloc, af_free, 1);
    afa_destroy();

    printf("\nchurn: %d random alloc/free ops over %d slots, per-call latency\n",
           CHURN_OPS, SLOTS);
    puts("  (clock_gettime adds a few tens of ns to every figure)");
    churn("glibc malloc/free", glibc_alloc, glibc_free);
    churn("afalloc/afree", af_alloc, af_free);
    afa_destroy();
    return 0;
}
