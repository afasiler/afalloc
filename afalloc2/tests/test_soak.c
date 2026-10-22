/* Soak test: does memory stay bounded? Heavy allocation and release in several rounds, with threads freeing
 * each other's blocks, checking that the allocator's own accounting returns to a small steady state and
 * that address space does not keep growing (a leak of slabs or of large mappings would show here).
 *   AFALLOC_SECURE=0|1|2 ./test_soak */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../afalloc.h"

static int failures;
static long scale = 1;                          /* AF_TEST_SCALE=N: 1/N of the work, footprint checks off (for ThreadSanitizer) */
static int footprint_checks = 1;
#define CHECK(cond) do { if (!(cond) && (footprint_checks || 0)) { printf("  FAIL  line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static size_t pick_size(uint64_t r)
{
    unsigned c = (unsigned)(r % 100);
    if (c < 60) return 16 + (size_t)((r >> 8) % 200);
    if (c < 90) return 216 + (size_t)((r >> 8) % 3000);
    if (c < 99) return 3216 + (size_t)((r >> 8) % 29000);
    return 32768 + (size_t)((r >> 8) % 500000);
}

#define N 200000
static long nblocks = N;
static void *blocks[N];

static void *worker(void *arg)
{
    long id = (long)arg;
    uint64_t s = 0x9E3779B97F4A7C15ull * (uint64_t)(id + 1);
    for (long i = id; i < nblocks; i += 4) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        blocks[i] = afalloc(pick_size(s));
        memset(blocks[i], (int)i, 16);
    }
    return NULL;
}

static void *freer(void *arg)
{
    long id = (long)arg;                           /* frees blocks that a different thread allocated */
    for (long i = (id + 1) % 4; i < nblocks; i += 4) {
        if (blocks[i]) { afree(blocks[i]); blocks[i] = NULL; }
    }
    return NULL;
}

/* ---- phase 2: producers stay alive while consumers free their blocks (the "owner must be told" path) ---- */
#define QN 512
typedef struct { void *p; } qitem_t;
static qitem_t ring[QN];
static unsigned qhead, qtail, qcount;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_full = PTHREAD_COND_INITIALIZER, not_empty = PTHREAD_COND_INITIALIZER;
static int prod_done;
static long per_producer = 300000;

static void *live_producer(void *arg)
{
    uint64_t s = 0x2545F4914F6CDD1Dull * (uint64_t)((long)arg + 1);
    for (long i = 0; i < per_producer; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        void *p = afalloc(16 + (size_t)((s >> 20) % 2000));
        pthread_mutex_lock(&qlock);
        while (qcount == QN) pthread_cond_wait(&not_full, &qlock);
        ring[qtail].p = p; qtail = (qtail + 1) % QN; qcount++;
        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&qlock);
    }
    return NULL;
}
static void *live_consumer(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&qlock);
        while (qcount == 0 && !prod_done) pthread_cond_wait(&not_empty, &qlock);
        if (qcount == 0) { pthread_mutex_unlock(&qlock); return NULL; }
        void *p = ring[qhead].p; qhead = (qhead + 1) % QN; qcount--;
        pthread_cond_signal(&not_full);
        pthread_mutex_unlock(&qlock);
        afree(p);
    }
}

static void phase_live(void)
{
    puts("phase 2: live producers, consumers free their blocks");
    afstats_t st;
    size_t first = 0;
    for (int round = 0; round < 4; round++) {
        pthread_t pt[3], ct[3];
        prod_done = 0;
        for (long i = 0; i < 3; i++) pthread_create(&pt[i], NULL, live_producer, (void *)i);
        for (int i = 0; i < 3; i++) pthread_create(&ct[i], NULL, live_consumer, NULL);
        for (int i = 0; i < 3; i++) pthread_join(pt[i], NULL);
        pthread_mutex_lock(&qlock);
        prod_done = 1;
        pthread_cond_broadcast(&not_empty);
        pthread_mutex_unlock(&qlock);
        for (int i = 0; i < 3; i++) pthread_join(ct[i], NULL);
        afget_stats(&st);
        printf("  round %d: committed so far %zu slabs\n", round, st.slabs_committed);
        if (round == 0) first = st.slabs_committed;
        if (round >= 2)
            CHECK(st.slabs_committed <= first + first / 10 + 8);
    }
}

/* ---- phase 3: can memory migrate between size classes? ----
 * 19 MB of 64-byte blocks are allocated and released in some way; then 19 MB of 1024-byte blocks are
 * allocated and kept alive while the number of slabs in use is read. If the 64-byte slabs went back to the
 * pool, only the 1 KB blocks need slabs (about 80); if they were lost or hoarded, about 76 more are in use. */
#define N64MAX 300000
#define N1KMAX 19000
static int N64 = N64MAX, N1K = N1KMAX;
#define NEED_1K 90                                  /* slabs the 1 KB blocks need, with some slack */
static void *mig[N64MAX];
static size_t in_use_now(void) { afstats_t st; afget_stats(&st); return st.slabs_in_use; }
static size_t g_peak;

static void *alloc_1k_measure(void *arg)
{
    (void)arg;
    static void *q[N1KMAX];
    for (int i = 0; i < N1K; i++) { q[i] = afalloc(1024); memset(q[i], 1, 8); }
    g_peak = in_use_now();
    for (int i = 0; i < N1K; i++) afree(q[i]);
    return NULL;
}
static void expect_migrated(const char *what, size_t base, size_t peak)
{
    printf("  %-72s %4zu slabs in use (baseline %zu)\n", what, peak, base);
    CHECK(peak <= base + NEED_1K);
}

/* a: same thread */
static void scenario_same_thread(void)
{
    size_t base = in_use_now();
    for (int i = 0; i < N64; i++) { mig[i] = afalloc(64); memset(mig[i], 1, 8); }
    for (int i = 0; i < N64; i++) afree(mig[i]);
    alloc_1k_measure(NULL);
    expect_migrated("same thread: free 64-byte blocks, then allocate 1 KB", base, g_peak);
}

/* b, c: another thread frees the blocks while the owner is alive (b) or while it exits (c) */
static pthread_mutex_t sm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sc = PTHREAD_COND_INITIALIZER;
static int state, owner_exits;
static void wait_state(int v) { pthread_mutex_lock(&sm); while (state < v) pthread_cond_wait(&sc, &sm); pthread_mutex_unlock(&sm); }
static void set_state(int v) { pthread_mutex_lock(&sm); state = v; pthread_cond_broadcast(&sc); pthread_mutex_unlock(&sm); }
static void *owner_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < N64; i++) { mig[i] = afalloc(64); memset(mig[i], 1, 8); }
    set_state(1);
    wait_state(2);
    if (!owner_exits)
        alloc_1k_measure(NULL);                     /* the owner itself reuses the memory that others freed */
    return NULL;
}
static void scenario_remote(int exits, const char *what)
{
    state = 0;
    owner_exits = exits;
    size_t base = in_use_now();
    pthread_t t;
    pthread_create(&t, NULL, owner_thread, NULL);
    wait_state(1);
    for (int i = 0; i < N64; i++) afree(mig[i]);  /* remote frees */
    set_state(2);
    pthread_join(t, NULL);
    if (exits) {                                    /* the owner exited: now another thread allocates 1 KB */
        pthread_create(&t, NULL, alloc_1k_measure, NULL);
        pthread_join(t, NULL);
    }
    expect_migrated(what, base, g_peak);
}

/* d: the owner exits holding its blocks; they are freed afterwards; a new thread adopts the slabs */
static void *exit_holding(void *arg)
{
    (void)arg;
    for (int i = 0; i < N64; i++) { mig[i] = afalloc(64); memset(mig[i], 1, 8); }
    return NULL;
}
static size_t g_adopt_peak;
static void *alloc_64_measure(void *arg)
{
    (void)arg;
    static void *q[N64MAX];
    for (int i = 0; i < N64; i++) { q[i] = afalloc(64); memset(q[i], 2, 8); }
    g_adopt_peak = in_use_now();
    for (int i = 0; i < N64; i++) afree(q[i]);
    return NULL;
}
static void scenario_adopt(void)
{
    size_t base = in_use_now();
    pthread_t t;
    pthread_create(&t, NULL, exit_holding, NULL);
    pthread_join(t, NULL);
    for (int i = 0; i < N64; i++) afree(mig[i]);
    pthread_create(&t, NULL, alloc_64_measure, NULL);
    pthread_join(t, NULL);
    printf("  %-72s %4zu slabs in use (baseline %zu)\n", "owner exited holding blocks; a new thread allocates the same class", g_adopt_peak, base);
    CHECK(g_adopt_peak <= base + 90);               /* the 64-byte blocks need about 76 slabs: the old ones must be reused */
}

/* e: remote frees of half the blocks, then the owner allocates the same class again */
static size_t g_half_peak;
static void *half_owner(void *arg)
{
    (void)arg;
    for (int i = 0; i < N64; i++) { mig[i] = afalloc(64); memset(mig[i], 1, 8); }
    set_state(1);
    wait_state(2);
    for (int i = 0; i < N64; i += 2) { mig[i] = afalloc(64); memset(mig[i], 3, 8); }
    g_half_peak = in_use_now();
    set_state(3);
    wait_state(4);
    for (int i = 0; i < N64; i++) afree(mig[i]);
    return NULL;
}
static void scenario_half(void)
{
    state = 0;
    size_t base = in_use_now();
    pthread_t t;
    pthread_create(&t, NULL, half_owner, NULL);
    wait_state(1);
    for (int i = 0; i < N64; i += 2) afree(mig[i]);    /* remote frees of every second block */
    set_state(2);
    wait_state(3);
    set_state(4);
    pthread_join(t, NULL);
    printf("  %-72s %4zu slabs in use (baseline %zu)\n", "remote frees half the blocks; the owner allocates that much again", g_half_peak, base);
    CHECK(g_half_peak <= base + 90);                /* without reuse of the freed half it would be about 114 */
}

/* f: the owner frees half the blocks itself while another thread frees the other half: the slab then has
   free blocks on its own list AND on the remote list at the same time */
static size_t g_mixed_peak;
static void *mixed_owner(void *arg)
{
    (void)arg;
    for (int i = 0; i < N64; i++) { mig[i] = afalloc(64); memset(mig[i], 1, 8); }
    set_state(1);
    for (int i = 0; i < N64; i += 2) afree(mig[i]);            /* local frees */
    set_state(2);
    wait_state(3);                                              /* main has freed the odd blocks (remote) */
    static void *q[N64MAX];
    for (int i = 0; i < N64; i++) { q[i] = afalloc(64); memset(q[i], 4, 8); }
    g_mixed_peak = in_use_now();
    for (int i = 0; i < N64; i++) afree(q[i]);
    return NULL;
}
static void scenario_mixed(void)
{
    state = 0;
    size_t base = in_use_now();
    pthread_t t;
    pthread_create(&t, NULL, mixed_owner, NULL);
    wait_state(2);
    for (int i = 1; i < N64; i += 2) afree(mig[i]);            /* remote frees of the other half */
    set_state(3);
    pthread_join(t, NULL);
    printf("  %-72s %4zu slabs in use (baseline %zu)\n", "owner frees half, another thread frees the other half, owner reallocates", g_mixed_peak, base);
    CHECK(g_mixed_peak <= base + 90);               /* losing either half would need about 38 more slabs */
}

/* g: the owner exits holding its blocks, ALL of them are freed afterwards by other threads: the slabs must
   go straight back to the pool (not wait for a thread that wants the same size class) */
static void scenario_abandoned_all_freed(void)
{
    size_t base = in_use_now();
    pthread_t t;
    pthread_create(&t, NULL, exit_holding, NULL);
    pthread_join(t, NULL);
    for (int i = 0; i < N64; i++) afree(mig[i]);
    afstats_t st;
    afget_stats(&st);
    printf("  %-72s %4zu abandoned slabs left\n", "owner exited holding blocks; every block freed afterwards", st.slabs_abandoned);
    /* Normally 0. A free that races with a thread's exit can leave one slab counted as live until a thread of the
       same size class adopts it, so a few slabs are tolerated; a broken return path leaves about 75. */
    CHECK(st.slabs_abandoned <= 4);
    pthread_create(&t, NULL, alloc_1k_measure, NULL);
    pthread_join(t, NULL);
    expect_migrated("  ...then another thread allocates 1 KB", base, g_peak);
}

/* h: the owner exits holding its blocks, half of them are freed afterwards, a new thread of the same class
   adopts the slabs and must reuse the freed half */
static size_t g_adopt_half_peak;
static void *alloc_half_measure(void *arg)
{
    (void)arg;
    static void *q[N64MAX];
    int n = N64 / 3;                                 /* fewer than the freed half, so some slabs stay on the abandoned list */
    for (int i = 0; i < n; i++) { q[i] = afalloc(64); memset(q[i], 2, 8); }
    g_adopt_half_peak = in_use_now();
    CHECK(afcheck() == 0);                          /* some abandoned slabs were adopted, the rest are still listed: the list must be intact */
    for (int i = 0; i < n; i++) afree(q[i]);
    return NULL;
}
static void scenario_adopt_half(void)
{
    size_t base = in_use_now();
    pthread_t t;
    pthread_create(&t, NULL, exit_holding, NULL);
    pthread_join(t, NULL);
    size_t held = in_use_now() - base;               /* the slabs the exited thread left behind */
    for (int i = 0; i < N64; i += 2) afree(mig[i]);
    pthread_create(&t, NULL, alloc_half_measure, NULL);
    pthread_join(t, NULL);
    for (int i = 1; i < N64; i += 2) afree(mig[i]);
    printf("  %-72s %4zu slabs in use (baseline %zu, left behind %zu)\n", "owner exited holding blocks; half freed; a new thread allocates a third as much", g_adopt_half_peak, base, held);
    CHECK(g_adopt_half_peak <= base + held + 12);    /* reusing the freed blocks needs no new slabs; ignoring them needs about held/3 more */
}

static void phase_migration(void)
{
    puts("phase 3: memory migrates between size classes (slabs in use)");
    scenario_same_thread();
    scenario_remote(0, "another thread frees the 64-byte blocks, the live owner then allocates 1 KB");
    scenario_remote(1, "another thread frees them, the owner exits, a new thread allocates 1 KB");
    scenario_adopt();
    scenario_half();
    scenario_mixed();
    scenario_abandoned_all_freed();
    scenario_adopt_half();
    CHECK(afcheck() == 0);
}

int main(void)
{
    const char *sc = getenv("AF_TEST_SCALE");
    if (sc && atol(sc) > 1) {
        scale = atol(sc);
        footprint_checks = 0;
        nblocks = N / scale; per_producer /= scale; N64 = N64MAX / (int)scale; N1K = N1KMAX / (int)scale;
    }
    afstats_t st;
    afree(afalloc(1));
    afget_stats(&st);
    printf("soak test, security level %d\n", st.security_level);
    size_t committed_after_first = 0;
    for (int round = 0; round < (scale > 1 ? 2 : 6); round++) {
        pthread_t t[4];
        for (long i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, (void *)i);
        for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
        afget_stats(&st);
        size_t peak_in_use = st.slabs_in_use;
        for (long i = 0; i < 4; i++) pthread_create(&t[i], NULL, freer, (void *)i);
        for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
        /* the workers exited, so their slabs were abandoned; a new thread adopts and reclaims them */
        pthread_t adopt;
        pthread_create(&adopt, NULL, worker, (void *)0);
        pthread_join(adopt, NULL);
        for (long i = 0; i < nblocks; i += 4) if (blocks[i]) { afree(blocks[i]); blocks[i] = NULL; }
        afget_stats(&st);
        printf("  round %d: peak %zu slabs in use, committed so far %zu, large live %zu, large bytes %zu MB\n",
               round, peak_in_use, st.slabs_committed, st.large_live, st.large_bytes >> 20);
        CHECK(afcheck() == 0);
        if (st.large_live != 0) { printf("  FAIL  large blocks leaked\n"); failures++; }
        if (round == 0) committed_after_first = st.slabs_committed;
        if (round >= 2)
            CHECK(st.slabs_committed <= committed_after_first + committed_after_first / 10 + 8);   /* no growth beyond 10% */
    }
    phase_live();
    phase_migration();
    if (failures) { printf("FAILED: %d\n", failures); return 1; }
    puts("soak test passed");
    return 0;
}
