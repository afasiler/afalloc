/* Runs the benchmark harness on a computer against the pool and against libc malloc, with a
 * fake cycle counter. This does not measure anything real; it checks that the harness
 * itself is sound (no crash, no out-of-bounds access under the sanitizers, sane counts)
 * before it is trusted on the board. */
#include <stdio.h>
#include <stdlib.h>
#include "bench_common.h"
#include "pool.h"

int main(void)
{
    pool_init();
    bench_run("pool (host, fake cycle counter)", pool_alloc, pool_free);
    bench_run("libc malloc (host, fake cycle counter)", malloc, free);
    return 0;
}
