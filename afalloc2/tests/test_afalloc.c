/* Functional and stress tests for afalloc2. Run at each security level:
 *   AFALLOC_SECURE=0 ./test_afalloc    (and 1, 2)
 * Every live block carries a pattern derived from its address and size; any overlap, lost write or
 * corruption by the allocator shows up as a pattern mismatch. */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../afalloc.h"

static int failures;
static long scale = 1;                        /* AF_TEST_SCALE=10 runs a tenth of the iterations (for slow tools like TSAN) */
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL  line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static uint64_t rng_s = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return rng_s; }

static void fill(void *p, size_t n, uint64_t tag)
{
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(tag + i * 31);
}
static int verify(const void *p, size_t n, uint64_t tag)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) if (b[i] != (uint8_t)(tag + i * 31)) return 0;
    return 1;
}

static void test_basic(void)
{
    puts("basic: sizes, alignment, usable size, contents");
    static const size_t sizes[] = {0, 1, 7, 8, 15, 16, 17, 24, 32, 33, 48, 63, 64, 65, 100, 127, 128, 129, 160, 200, 255, 256,
        257, 500, 1000, 1024, 1025, 2000, 4096, 5000, 8191, 8192, 10000, 16384, 20000, 32767, 32768, 32769, 40000,
        65536, 100000, 1000000, 5000000};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        size_t n = sizes[i];
        void *p = afalloc(n);
        CHECK(p != NULL);
        if (!p) continue;
        CHECK(((uintptr_t)p & 15) == 0);
        CHECK(afusable_size(p) >= n);
        fill(p, n, i);
        CHECK(verify(p, n, i));
        afree(p);
    }
    afree(NULL);
    CHECK(afusable_size(NULL) == 0);
}

static void test_calloc_realloc_aligned(void)
{
    puts("calloc, realloc, aligned_alloc");
    for (size_t n = 1; n < 3000000; n = n * 3 + 1) {           /* small, medium and large; large ones come back from the cache */
        uint8_t *p = afcalloc(n, 1);
        CHECK(p != NULL);
        int zero = 1;
        for (size_t i = 0; i < n; i++) zero &= p[i] == 0;
        CHECK(zero);
        memset(p, 0xAB, n);
        afree(p);
        p = afcalloc(n, 1);                        /* a recycled block must be zero again */
        zero = 1;
        for (size_t i = 0; i < n; i++) zero &= p[i] == 0;
        CHECK(zero);
        afree(p);
    }
    CHECK(afcalloc((size_t)-1, 2) == NULL);
    CHECK(errno == ENOMEM);
    CHECK(afcalloc(((size_t)-1) / 2 + 2, 2) == NULL);        /* n * m wraps around to 2: must not return a 2-byte block */
    CHECK(afcalloc(((size_t)1 << 33) + 1, (size_t)1 << 31) == NULL);

    uint8_t *p = afalloc(10);
    fill(p, 10, 5);
    size_t cur = 10;
    for (size_t n = 11; n < 3000000; n = n * 2 + 1) {   /* grows through small, medium and large */
        p = afrealloc(p, n);
        CHECK(p != NULL);
        CHECK(verify(p, cur, 5));
        fill(p, n, 5);
        cur = n;
    }
    for (size_t n = cur; n > 1; n /= 3) {                /* and back down */
        p = afrealloc(p, n);
        CHECK(p != NULL);
        CHECK(verify(p, n, 5));
        fill(p, n, 5);
    }
    afree(p);
    CHECK(afrealloc(NULL, 100) != NULL);

    static const size_t aligns[] = {16, 32, 64, 128, 256, 1024, 4096, 16384, 65536};
    for (size_t i = 0; i < sizeof aligns / sizeof *aligns; i++)
        for (size_t n = 1; n < 100000; n = n * 7 + 3) {
            void *q = afaligned_alloc(aligns[i], n);
            CHECK(q != NULL);
            CHECK(((uintptr_t)q & (aligns[i] - 1)) == 0);
            CHECK(afusable_size(q) >= n);
            fill(q, n, 9);
            CHECK(verify(q, n, 9));
            afree(q);
        }
    CHECK(afaligned_alloc(3, 10) == NULL);
}

#define NSLOT 4096
static void test_random_stress(void)
{
    puts("random alloc/free/realloc stress with pattern checks");
    static void *slot[NSLOT];
    static size_t size[NSLOT];
    static uint64_t tag[NSLOT];
    for (long step = 0; step < 600000 / scale; step++) {
        unsigned i = (unsigned)(rnd() % NSLOT);
        uint64_t r = rnd();
        size_t n = (r & 7) == 0 ? (size_t)(rnd() % 40000) + 1 : (size_t)(rnd() % 300) + 1;
        if (!slot[i]) {
            slot[i] = afalloc(n);
            CHECK(slot[i] != NULL);
            size[i] = n; tag[i] = rnd();
            fill(slot[i], n, tag[i]);
        } else if ((r >> 8) % 4 == 0) {
            CHECK(verify(slot[i], size[i], tag[i]));
            slot[i] = afrealloc(slot[i], n);
            CHECK(slot[i] != NULL);
            size_t keep = n < size[i] ? n : size[i];
            CHECK(verify(slot[i], keep, tag[i]));
            size[i] = n;
            fill(slot[i], n, tag[i]);
        } else {
            CHECK(verify(slot[i], size[i], tag[i]));
            afree(slot[i]);
            slot[i] = NULL;
        }
        if (failures > 20) { puts("  too many failures, stopping"); return; }
    }
    for (unsigned i = 0; i < NSLOT; i++)
        if (slot[i]) { CHECK(verify(slot[i], size[i], tag[i])); afree(slot[i]); }
}

/* ---- threads ---- */
#define QN 256
typedef struct { void *p; size_t n; uint64_t tag; } item_t;
static item_t ring[QN];
static unsigned qhead, qtail, qcount;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_full = PTHREAD_COND_INITIALIZER, not_empty = PTHREAD_COND_INITIALIZER;
static int producers_done;
static _Atomic long bad_pattern, handed, consumed;

static void *producer(void *arg)
{
    uint64_t s = (uint64_t)(uintptr_t)arg * 0x2545F4914F6CDD1Dull + 1;
    for (int k = 0; k < 40000 / scale; k++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        size_t n = (s & 15) == 0 ? (size_t)(s >> 20) % 20000 + 1 : (size_t)(s >> 20) % 200 + 1;
        void *p = afalloc(n);
        fill(p, n, s);
        pthread_mutex_lock(&qlock);
        while (qcount == QN) pthread_cond_wait(&not_full, &qlock);
        ring[qtail] = (item_t){p, n, s};
        qtail = (qtail + 1) % QN;
        qcount++;
        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&qlock);
        atomic_fetch_add(&handed, 1);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&qlock);
        while (qcount == 0 && !producers_done) pthread_cond_wait(&not_empty, &qlock);
        if (qcount == 0) { pthread_mutex_unlock(&qlock); return NULL; }
        item_t it = ring[qhead];
        qhead = (qhead + 1) % QN;
        qcount--;
        pthread_cond_signal(&not_full);
        pthread_mutex_unlock(&qlock);
        if (!verify(it.p, it.n, it.tag)) atomic_fetch_add(&bad_pattern, 1);
        afree(it.p);                               /* freed by a different thread than the one that allocated */
        atomic_fetch_add(&consumed, 1);
    }
}

static void test_cross_thread(void)
{
    puts("cross-thread: 4 producers allocate, 4 consumers free");
    pthread_t pt[4], ct[4];
    for (long i = 0; i < 4; i++) pthread_create(&pt[i], NULL, producer, (void *)(i + 1));
    for (long i = 0; i < 4; i++) pthread_create(&ct[i], NULL, consumer, NULL);
    for (int i = 0; i < 4; i++) pthread_join(pt[i], NULL);
    pthread_mutex_lock(&qlock);
    producers_done = 1;
    pthread_cond_broadcast(&not_empty);
    pthread_mutex_unlock(&qlock);
    for (int i = 0; i < 4; i++) pthread_join(ct[i], NULL);
    CHECK(bad_pattern == 0);
    CHECK(handed == consumed);
    afstats_t st; afget_stats(&st);
    CHECK(st.large_live == 0);
}

#define ORPHANS 3000
static void *orphans[ORPHANS];
static size_t orphan_size[ORPHANS];
static void *thread_that_exits(void *arg)
{
    (void)arg;
    for (int i = 0; i < ORPHANS; i++) {
        size_t n = (size_t)(i % 500) + 1;
        orphans[i] = afalloc(n);
        orphan_size[i] = n;
        fill(orphans[i], n, (uint64_t)i);
    }
    return NULL;                                     /* exits holding all those blocks */
}
static void *adopter(void *arg)
{
    (void)arg;
    void *q[2000];
    for (int round = 0; round < (int)(20 / (scale > 5 ? 5 : scale)); round++) {
        for (int i = 0; i < 2000; i++) { q[i] = afalloc((size_t)(i % 500) + 1); fill(q[i], (size_t)(i % 500) + 1, 77); }
        for (int i = 0; i < 2000; i++) { CHECK(verify(q[i], (size_t)(i % 500) + 1, 77)); afree(q[i]); }
    }
    return NULL;
}

static void test_thread_exit(void)
{
    puts("thread exit: blocks outlive their thread, are freed by others, slabs get adopted");
    pthread_t t;
    pthread_create(&t, NULL, thread_that_exits, NULL);
    pthread_join(t, NULL);
    pthread_t a[4];
    for (int i = 0; i < 4; i++) pthread_create(&a[i], NULL, adopter, NULL);
    for (int i = 0; i < ORPHANS; i++) {
        CHECK(verify(orphans[i], orphan_size[i], (uint64_t)i));
        afree(orphans[i]);
    }
    for (int i = 0; i < 4; i++) pthread_join(a[i], NULL);
    for (int round = 0; round < (int)(50 / scale); round++) {      /* many short-lived threads: heaps are recycled */
        pthread_t th[8];
        for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, adopter, NULL);
        for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    }
}

static void test_large(void)
{
    puts("large blocks: map, contents, reuse, statistics");
    afstats_t before; afget_stats(&before);
    void *p[40];
    for (int i = 0; i < 40; i++) {
        size_t n = 40000 + (size_t)i * 70001;
        p[i] = afalloc(n);
        CHECK(p[i] != NULL);
        fill(p[i], n, (uint64_t)i);
    }
    afstats_t mid; afget_stats(&mid);
    CHECK(mid.large_live == before.large_live + 40);
    for (int i = 0; i < 40; i++) {
        size_t n = 40000 + (size_t)i * 70001;
        CHECK(verify(p[i], n, (uint64_t)i));
        afree(p[i]);
    }
    afstats_t after; afget_stats(&after);
    CHECK(after.large_live == before.large_live);
    CHECK(after.large_bytes == before.large_bytes);
}

int main(void)
{
    const char *sc = getenv("AF_TEST_SCALE");
    if (sc && atol(sc) > 0) scale = atol(sc);
    afstats_t st;
    void *warm = afalloc(1);
    afget_stats(&st);
    printf("afalloc2 tests, security level %d\n", st.security_level);
    afree(warm);
    test_basic();
    test_calloc_realloc_aligned();
    test_random_stress();
    test_large();
    test_cross_thread();
    test_thread_exit();
    CHECK(afcheck() == 0);
    if (failures) { printf("FAILED: %d\n", failures); return 1; }
    puts("all tests passed");
    return 0;
}
