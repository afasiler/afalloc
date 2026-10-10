#ifndef MMAP_ALLOCATOR_H
#define MMAP_ALLOCATOR_H

#include <stddef.h>

void *afalloc(size_t size);
void  afree(void *ptr);
void  afa_coalesce(void);
void  afa_reset(void);

#endif