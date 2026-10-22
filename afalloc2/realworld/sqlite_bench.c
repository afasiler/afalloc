/* SQLite (the system library, 3.x) with its memory allocator replaced through the official
 * sqlite3_config(SQLITE_CONFIG_MALLOC) hook. Everything runs in an in-memory database so only the CPU and the
 * allocator matter. Mode (argv[1]):
 *   default   SQLite's own default (it calls the system malloc directly)
 *   sys       the system malloc through the same hook (the fair baseline for "af")
 *   af        afalloc through the hook
 *   count     counts calls, then uses the system malloc
 * Prints "metric,value" lines: the time of each phase in ms and a checksum that must match across modes. */
#include <malloc/malloc.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../afalloc.h"

static unsigned long calls, bytes;
static void *s_malloc(int n) { return malloc((size_t)n); }
static void s_free(void *p) { free(p); }
static void *s_realloc(void *p, int n) { return realloc(p, (size_t)n); }
static int s_size(void *p) { return p ? (int)malloc_size(p) : 0; }
static int s_round(int n) { return (n + 15) & ~15; }
static int s_init(void *a) { (void)a; return SQLITE_OK; }
static void s_shut(void *a) { (void)a; }
static void *c_malloc(int n) { calls++; bytes += (unsigned long)n; return malloc((size_t)n); }
static void *c_realloc(void *p, int n) { calls++; bytes += (unsigned long)n; return realloc(p, (size_t)n); }
static void *a_malloc(int n) { return afalloc((size_t)n); }
static void a_free(void *p) { afree(p); }
static void *a_realloc(void *p, int n) { return afrealloc(p, (size_t)n); }
static int a_size(void *p) { return (int)afusable_size(p); }

static double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
static void metric(const char *n, double v) { printf("%s,%.3f\n", n, v); fflush(stdout); }

static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static void ok(int rc, sqlite3 *db) { if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) { fprintf(stderr, "sqlite error %d: %s\n", rc, sqlite3_errmsg(db)); exit(1); } }
static void exec(sqlite3 *db, const char *sql) { char *e = 0; int rc = sqlite3_exec(db, sql, 0, 0, &e); if (rc) { fprintf(stderr, "exec failed: %s (%s)\n", e, sql); exit(1); } }

static double checksum;
static int add_row(void *arg, int ncol, char **v, char **n) { (void)arg; (void)n; for (int i = 0; i < ncol; i++) if (v[i]) checksum += (double)strlen(v[i]) + (v[i][0] ? v[i][0] : 0); return 0; }
static void query(sqlite3 *db, const char *sql) { char *e = 0; int rc = sqlite3_exec(db, sql, add_row, 0, &e); if (rc) { fprintf(stderr, "query failed: %s\n", e); exit(1); } }

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "default";
    static sqlite3_mem_methods m;
    int configure = 1;
    if (!strcmp(mode, "default")) configure = 0;
    else if (!strcmp(mode, "sys")) m = (sqlite3_mem_methods){ s_malloc, s_free, s_realloc, s_size, s_round, s_init, s_shut, 0 };
    else if (!strcmp(mode, "af")) m = (sqlite3_mem_methods){ a_malloc, a_free, a_realloc, a_size, s_round, s_init, s_shut, 0 };
    else if (!strcmp(mode, "count")) m = (sqlite3_mem_methods){ c_malloc, s_free, c_realloc, s_size, s_round, s_init, s_shut, 0 };
    else { fprintf(stderr, "mode?\n"); return 2; }
    if (configure && sqlite3_config(SQLITE_CONFIG_MALLOC, &m) != SQLITE_OK) { fprintf(stderr, "sqlite3_config failed\n"); return 2; }

    sqlite3 *db;
    ok(sqlite3_open(":memory:", &db), db);
    exec(db, "PRAGMA cache_size=-400000; PRAGMA temp_store=MEMORY; PRAGMA synchronous=OFF;");
    exec(db, "CREATE TABLE t1(id INTEGER PRIMARY KEY, a INTEGER, b TEXT, c REAL);"
             "CREATE TABLE t2(id INTEGER PRIMARY KEY, a INTEGER, d TEXT);");
    const int N1 = 250000, N2 = 80000;
    double total0 = now_ms(), t;

    t = now_ms();
    exec(db, "BEGIN");
    sqlite3_stmt *st;
    ok(sqlite3_prepare_v2(db, "INSERT INTO t1(a,b,c) VALUES(?1,?2,?3)", -1, &st, 0), db);
    for (int i = 0; i < N1; i++) {
        char b[96];
        int len = snprintf(b, sizeof b, "row-%d-%llx-%.*s", i, (unsigned long long)(rnd() & 0xFFFFFF), (int)(rnd() % 40), "abcdefghijklmnopqrstuvwxyz0123456789ABCD");
        sqlite3_bind_int(st, 1, (int)(rnd() % 50000));
        sqlite3_bind_text(st, 2, b, len, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 3, (double)(rnd() % 100000) / 100.0);
        ok(sqlite3_step(st), db);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    ok(sqlite3_prepare_v2(db, "INSERT INTO t2(a,d) VALUES(?1,?2)", -1, &st, 0), db);
    for (int i = 0; i < N2; i++) {
        char d[48];
        int len = snprintf(d, sizeof d, "d%d_%llu", i, (unsigned long long)(rnd() % 1000000));
        sqlite3_bind_int(st, 1, (int)(rnd() % 50000));
        sqlite3_bind_text(st, 2, d, len, SQLITE_TRANSIENT);
        ok(sqlite3_step(st), db);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    exec(db, "COMMIT");
    metric("sqlite_insert_ms", now_ms() - t);

    t = now_ms();
    exec(db, "CREATE INDEX i1 ON t1(a); CREATE INDEX i2 ON t1(b); CREATE INDEX i3 ON t2(a);");
    metric("sqlite_index_ms", now_ms() - t);

    t = now_ms();
    ok(sqlite3_prepare_v2(db, "SELECT c, b FROM t1 WHERE a=?1", -1, &st, 0), db);
    for (int i = 0; i < 60000; i++) {
        sqlite3_bind_int(st, 1, (int)(rnd() % 50000));
        while (sqlite3_step(st) == SQLITE_ROW) checksum += sqlite3_column_double(st, 0) + sqlite3_column_bytes(st, 1);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    metric("sqlite_lookup_ms", now_ms() - t);

    t = now_ms();
    for (int i = 0; i < 4; i++) query(db, "SELECT substr(b,1,4) k, count(*), avg(c), max(length(b)) FROM t1 GROUP BY k ORDER BY 2 DESC, 1 LIMIT 50");
    query(db, "SELECT a, count(*) FROM t1 GROUP BY a ORDER BY 2 DESC, 1 LIMIT 20");
    query(db, "SELECT b FROM t1 ORDER BY c DESC, id LIMIT 100");
    metric("sqlite_group_sort_ms", now_ms() - t);

    t = now_ms();
    query(db, "SELECT count(*), sum(t1.c) FROM t1 JOIN t2 ON t1.a=t2.a WHERE t2.id%7=0");
    query(db, "SELECT t2.d, count(*) FROM t2 JOIN t1 ON t1.a=t2.a WHERE t1.id%97=0 GROUP BY t2.d ORDER BY 2 DESC, 1 LIMIT 10");
    metric("sqlite_join_ms", now_ms() - t);

    t = now_ms();
    query(db, "SELECT count(*) FROM t1 WHERE b LIKE '%a5f%'");
    query(db, "SELECT count(*), sum(length(upper(b) || lower(b) || replace(b,'-','+'))) FROM t1 WHERE id%3=0");
    metric("sqlite_text_ms", now_ms() - t);

    t = now_ms();
    exec(db, "BEGIN; UPDATE t1 SET c=c*1.01 WHERE a%7=0; UPDATE t1 SET b=b||'-upd' WHERE a%5=0; DELETE FROM t1 WHERE a%11=0; COMMIT;");
    metric("sqlite_update_delete_ms", now_ms() - t);

    t = now_ms();
    for (int i = 0; i < 1500; i++) {                           /* many small statements: prepare, run, finalize */
        char sql[160];
        snprintf(sql, sizeof sql, "SELECT count(*), max(c) FROM t1 WHERE a BETWEEN %d AND %d", i * 7, i * 7 + 20);
        query(db, sql);
    }
    metric("sqlite_small_stmts_ms", now_ms() - t);

    metric("sqlite_total_ms", now_ms() - total0);
    metric("checksum", checksum);
    sqlite3_close(db);
    if (!strcmp(mode, "count")) { printf("alloc_calls,%lu\nalloc_bytes_MB,%.1f\n", calls, (double)bytes / 1048576.0); }
    return 0;
}
