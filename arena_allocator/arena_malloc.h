#ifndef ARENA_MALLOC_H
#define ARENA_MALLOC_H

#include <stddef.h>

void *afalloc(size_t size);
void  arena_free(void *ptr);   /* user pointer from afalloc(); coalesces both ways */
void  afa_reset(void);

#endif
