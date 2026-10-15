/* Host unit test: builds the pool with ordinary static arrays (POOL_HOST), runs the same
 * self-test the board runs, then repeats it a few times from a clean pool to make sure
 * pool_init() fully resets the state. Build with the sanitizers: any out-of-bounds write
 * by the pool shows up as an error. */
#include <stdio.h>
#include "pool.h"
#include "pool_config.h"
#include "pool_selftest.h"

static void log_line(const char *name, int failed, int line)
{
    if (failed)
        printf("  FAIL  %s (first failed check at pool_selftest.c:%d)\n", name, line);
    else
        printf("  ok    %s\n", name);
}

int main(void)
{
    int total = 0;
    pool_init();
    printf("pool selftest (host): %u pools, %lu bytes of blocks, POOL_DEBUG=%d POOL_SPILL=%d\n",
           pool_count(), (unsigned long)pool_total_bytes(), POOL_DEBUG, POOL_SPILL);
    for (int round = 1; round <= 3; round++) {
        printf("round %d\n", round);
        pool_init();
        total += pool_selftest(log_line);
    }
    if (total) {
        printf("FAILED: %d test(s)\n", total);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
