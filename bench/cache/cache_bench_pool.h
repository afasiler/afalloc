/* Pool table for the cache experiment: one huge pool of 64-byte blocks (half a cache line on Apple Silicon, whose lines are 128 bytes),
 * so the working set can be swept from 16 KB to 64 MB with a single size class. */
#ifndef CACHE_BENCH_POOL_H
#define CACHE_BENCH_POOL_H

#define POOL_TABLE(X) X(64, 1048640)
#define POOL_MAX_TOTAL_BYTES (128u * 1024u * 1024u)

#endif
