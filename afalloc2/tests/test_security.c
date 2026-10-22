/* Security tests: each case commits a memory-safety bug in a child process and the parent checks whether
 * afalloc2 caught it (the child dies from SIGABRT/SIGSEGV/SIGBUS) at each security level.
 *
 * "caught" is asserted only from the level where it is promised; below that the result is reported so
 * the table shows exactly what each level buys. Nothing here calls afalloc in the parent process. */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../afalloc.h"

#define SLAB (256u * 1024u)

typedef struct {
    const char *name;
    void (*run)(void);
    int must_catch_from;          /* lowest level at which the bug must be caught; 3 = never promised */
    const char *what;
    int property;                 /* 1: not a bug to catch but a guarantee: the child must exit cleanly from that level */
    int skip_level2;              /* 1: level 2 may absorb the bug harmlessly (quarantine), so no detection is promised */
    const char *msg;              /* the allocator must abort with a message containing this; NULL = a guard-page fault */
} case_t;

static void c_double_free_now(void) { void *p = afalloc(64); afree(p); afree(p); }
static void c_double_free_later(void)
{
    void *p = afalloc(64), *q = afalloc(64);
    afree(p); afree(q); afree(p);
}
static void c_interior_free(void) { char *p = afalloc(64); afree(p + 8); }
static void c_foreign_free(void) { static char x[64]; afree(x + 16); }
static void c_unused_heap_free(void)
{
    char *p = afalloc(64);
    uintptr_t base = (uintptr_t)p & ~(uintptr_t)(SLAB - 1);
    afree((void *)(base + 100 * (uintptr_t)SLAB + 64));
}
static void c_freelist_overwrite(void)
{
    uint64_t *p = afalloc(64);
    afree(p);
    *p = 0x4141414141414141ull;                    /* write after free: forge the free-list link */
    void *a = afalloc(64);                       /* returns p */
    void *b = afalloc(64);                       /* would return the attacker-chosen address */
    if ((uintptr_t)b == 0x4141414141414141ull)
        _exit(7);                                  /* the allocator handed out a forged pointer */
    (void)a;
}
/* Fill a fresh slab completely with the size class whose blocks tile it exactly (4096 B on 4 KiB pages,
   16384 B on 16 KiB pages), then overflow the last block by one byte: the next byte is the guard page. */
static void c_freelist_bitflip(void)               /* partial overwrite: XOR high bits into the stored link, no key needed */
{
    uint64_t *p = afalloc(64);
    afree(p);
    *p ^= 0x3FFF0ull;                              /* moves the decoded link inside the slab but past the last block */
    void *a = afalloc(64);                       /* returns p; its link now points outside the blocks */
    void *b = afalloc(64);
    (void)a; (void)b;
}
static void c_slab_overflow(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t size = page == 4096 ? 4096 : 16384;
    size_t blocks = (SLAB - page) / size;
    char *last = NULL;                              /* the block with the highest address (level 2 hands blocks out in random order) */
    for (size_t i = 0; i < blocks; i++) {
        char *p = afalloc(size);
        if (p > last) last = p;
    }
    last[size] = 1;
}
static void *remote_interior_thread(void *arg) { afree((char *)arg + 8); return NULL; }
static void c_remote_interior_free(void)
{
    char *p = afalloc(64);
    pthread_t t;
    pthread_create(&t, NULL, remote_interior_thread, p);
    pthread_join(t, NULL);
}
static void c_large_gap(void)                      /* property: large blocks are not adjacent (guard pages between them) */
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t a[16];
    for (int i = 0; i < 16; i++) a[i] = (uintptr_t)afalloc(100000);
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            if (i != j && a[j] >= a[i] && a[j] - a[i] < afusable_size((void *)a[i]) + page)
                _exit(9);                         /* closer than data + one guard page */
    _exit(0);
}
static void c_large_overflow(void)
{
    char *p = afalloc(100000);
    p[afusable_size(p)] = 1;                      /* one byte past the end */
}
static void c_large_underflow(void)
{
    char *p = afalloc(100000);
    p[-1] = 1;
}
static void c_large_double_free(void) { void *p = afalloc(100000); afree(p); afree(p); }
static void c_uaf_write(void)
{
    char *p = afalloc(64);
    afree(p);
    p[20] = 1;                                     /* write into the freed block */
    for (int i = 0; i < 100; i++) afree(afalloc(64));   /* cycle the quarantine until p is released */
}
static void c_recycled_zero(void)                  /* property: a recycled block holds no stale data, not even the old free-list link */
{
    enum { K = 400 };
    char *a[K];
    for (int i = 0; i < K; i++) { a[i] = afalloc(64); memset(a[i], 0x77, 64); }
    for (int i = 0; i < K; i++) afree(a[i]);
    for (int i = 0; i < K; i++) afree(afalloc(64));            /* cycle the quarantine so every block is released */
    for (int i = 0; i < K; i++) {
        a[i] = afalloc(64);
        for (int j = 0; j < 64; j++)
            if (a[i][j]) _exit(9);
    }
    _exit(0);
}
static void c_uaf_read_zero(void)
{
    char *p = afalloc(100);
    memset(p, 0x5A, 100);
    afree(p);
    _exit(p[50] == 0 ? 0 : 9);                     /* exit 9: the freed data was still readable */
}
static void c_realloc_foreign(void) { static char x[64]; afrealloc(x, 128); }

static const case_t CASES[] = {
    { "double free (immediate)", c_double_free_now, 1, "free(p); free(p)", 0, 0, "double free" },
    { "double free (after another free)", c_double_free_later, 2, "free(p); free(q); free(p)", 0, 0, "double free" },
    { "free of an interior pointer", c_interior_free, 1, "free(p + 8)", 0, 0, "invalid pointer" },
    { "free of an interior pointer from another thread", c_remote_interior_free, 1, "other thread frees p + 8", 0, 0, "invalid pointer" },
    { "free of a foreign pointer", c_foreign_free, 1, "free(&global)", 0, 0, "not allocated by afalloc" },
    { "free into an unused heap region", c_unused_heap_free, 1, "free(wild pointer inside the heap range)", 0, 0, "unused part of the heap" },
    { "forged free-list link (use after free)", c_freelist_overwrite, 1, "write to a freed block, then allocate twice", 0, 1, "corrupted free list" },
    { "free-list link damaged by a partial overwrite", c_freelist_bitflip, 1, "flip high bits of a freed block's link", 0, 1, "corrupted free list" },
    { "slab: last block overflows into the guard", c_slab_overflow, 1, "write one byte past the last block", 0, 0, NULL },
    { "large block overflow by one byte", c_large_overflow, 1, "p[usable] = 1", 0, 0, NULL },
    { "large block underflow by one byte", c_large_underflow, 1, "p[-1] = 1", 0, 0, NULL },
    { "large blocks are separated by guard pages", c_large_gap, 1, "address gap between large blocks", 1, 0, NULL },
    { "large block double free", c_large_double_free, 1, "free(p); free(p)", 0, 0, "not allocated by afalloc" },
    { "write after free (caught on release)", c_uaf_write, 2, "write into a quarantined block", 0, 0, "write after free" },
    { "recycled blocks hold no stale data", c_recycled_zero, 2, "allocate again after free, read the block", 1, 0, NULL },
    { "freed data is zeroed (read after free)", c_uaf_read_zero, 2, "free(p); read p[50]", 1, 0, NULL },
    { "realloc of a foreign pointer", c_realloc_foreign, 1, "realloc(&global, n)", 0, 0, "not allocated by afalloc" },
};
#define NCASES (sizeof CASES / sizeof *CASES)

/* returns 1 caught, 0 survived, 2 survived but misbehaved (exit code 7 or 9) */
static int run_case(const case_t *c, int level, char *msg, size_t msglen, int *sig_out)
{
    int pipefd[2];
    fflush(stdout);                                 /* or the child's abort() would flush our buffer again */
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        char lv[2] = { (char)('0' + level), 0 };
        setenv("AFALLOC_SECURE", lv, 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]); close(pipefd[1]);
        alarm(20);
        c->run();
        _exit(0);
    }
    close(pipefd[1]);
    ssize_t n = read(pipefd[0], msg, msglen - 1);
    msg[n > 0 ? n : 0] = 0;
    char *nl = strchr(msg, '\n');
    if (nl) *nl = 0;
    close(pipefd[0]);
    int st;
    waitpid(pid, &st, 0);
    if (sig_out) *sig_out = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
    if (WIFSIGNALED(st)) {
        int s = WTERMSIG(st);
        if (s == SIGABRT || s == SIGSEGV || s == SIGBUS) {
            if (!msg[0]) snprintf(msg, msglen, s == SIGSEGV || s == SIGBUS ? "guard page hit (signal %d)" : "aborted (signal %d)", s);
            return 1;
        }
        return -1;
    }
    if (WIFEXITED(st) && (WEXITSTATUS(st) == 7 || WEXITSTATUS(st) == 9)) return 2;
    return 0;
}

int main(void)
{
    int failures = 0;
    printf("%-42s | %-9s | %-9s | %-9s\n", "bug", "level 0", "level 1", "level 2");
    printf("%.42s-+-%.9s-+-%.9s-+-%.9s\n", "------------------------------------------", "---------", "---------", "---------");
    for (unsigned i = 0; i < NCASES; i++) {
        char cell[3][12];
        char msg[3][160];
        for (int level = 0; level < 3; level++) {
            int sig = 0;
            int r = run_case(&CASES[i], level, msg[level], sizeof msg[level], &sig);
            const char *label = CASES[i].property ? (r == 0 ? "clean" : "MISUSED")
                              : (r == 1 ? "caught" : r == 2 ? "MISUSED" : r == 0 ? "missed" : "error");
            if (!CASES[i].property && r == 0 && CASES[i].skip_level2 && level == 2) label = "absorbed";
            snprintf(cell[level], sizeof cell[level], "%s", label);
            int promised = level >= CASES[i].must_catch_from && !(CASES[i].skip_level2 && level == 2);
            int ok;
            if (CASES[i].property)
                ok = r == 0;
            else if (CASES[i].msg)                      /* the allocator itself must have noticed and said so */
                ok = r == 1 && sig == SIGABRT && strstr(msg[level], CASES[i].msg) != NULL;
            else                                        /* a guard page must have stopped the write */
                ok = r == 1 && (sig == SIGSEGV || sig == SIGBUS);
            if (promised && !ok) {
                failures++;
                printf("  FAIL: '%s' must %s at level %d, got %s\n", CASES[i].name,
                       CASES[i].property ? "hold" : "be caught", level, label);
            }
            if (!CASES[i].property && r == 2) {         /* handing out a forged pointer is never acceptable at level >= 1 */
                if (level >= 1) { failures++; printf("  FAIL: '%s' handed out a forged pointer at level %d\n", CASES[i].name, level); }
            }
        }
        printf("%-42s | %-9s | %-9s | %-9s\n", CASES[i].name, cell[0], cell[1], cell[2]);
    }
    puts("\nmessages at level 2:");
    for (unsigned i = 0; i < NCASES; i++) {
        char msg[160];
        run_case(&CASES[i], 2, msg, sizeof msg, NULL);
        printf("  %-42s %s\n", CASES[i].name, msg[0] ? msg : "(no message)");
    }
    if (failures) { printf("FAILED: %d\n", failures); return 1; }
    puts("all promised detections hold");
    return 0;
}
