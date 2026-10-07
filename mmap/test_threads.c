#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "afalloc_persistent.h"

#define THREADS 8
#define ITERS 20000
#define SLOTS 64

/* Each thread allocates, stamps with its own id, frees and reallocates.
   Under TSan any unsynchronised allocator state is reported; the stamps catch
   two threads being handed the same block. */
static void *worker(void *arg){
    unsigned char id = (unsigned char)(uintptr_t)arg;
    unsigned char *slot[SLOTS] = {0};
    size_t len[SLOTS] = {0};
    unsigned seed = id * 7919u + 1;
    for (int i = 0; i < ITERS; i++) {
        int s = rand_r(&seed) % SLOTS;
        if (slot[s]) {
            for (size_t j = 0; j < len[s]; j++) assert(slot[s][j] == id);
            afree(slot[s]);
            slot[s] = NULL;
        } else {
            len[s] = 1 + rand_r(&seed) % 600;
            slot[s] = (i & 1) ? afalloc(len[s]) : afalloc_persistent(len[s]);
            assert(slot[s] != NULL);
            memset(slot[s], id, len[s]);
        }
    }
    for (int s = 0; s < SLOTS; s++) {
        if (!slot[s]) continue;
        for (size_t j = 0; j < len[s]; j++) assert(slot[s][j] == id);
        afree(slot[s]);
    }
    return NULL;
}

int main(void){
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        assert(pthread_create(&t[i], NULL, worker, (void*)(uintptr_t)(i + 1)) == 0);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    afa_destroy();
    puts("test_threads: all tests passed");
    return 0;
}
