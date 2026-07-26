#ifndef MMAP_ALLOCATOR_H
#define MMAP_ALLOCATOR_H

#include <stddef.h>

void *afalloc(size_t size);
void  f_free(void *ptr);
void  f_coalescing(void);
void  reset_region(void);

#endif