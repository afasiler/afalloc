/* Self-test for the pool allocator. The same code runs on the Mac (host unit test, with
 * address/undefined-behaviour sanitizers) and on the board (app_selftest), so what is
 * proven on the host is exercised again on the real hardware. */
#ifndef POOL_SELFTEST_H
#define POOL_SELFTEST_H

/* Called once per test: name, status (0 = pass, 1 = fail) and, on failure, the source
 * line of the first failed check. */
typedef void (*selftest_log_t)(const char *name, int failed, int line);

/* Runs every test. Returns the number of failed tests (0 = all good). */
int pool_selftest(selftest_log_t log);

#endif
