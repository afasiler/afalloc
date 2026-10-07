/*
custom arena allocator for luckfox pico mini a rv1103
but it can work through all mcu and low level computer to get max performance with allocation.
*/


#include <stddef.h>

/* Chunk/region size in bytes; build with -DAFA_SIZE=<bytes> to change it. */
#ifndef AFA_SIZE
#define AFA_SIZE ((size_t)1024 * 1024)
#endif
#define MEM_SIZE ((size_t)AFA_SIZE)

/* two size_t fields keep the header a multiple of 8 bytes on 32-bit and
   64-bit targets, so user pointers stay 8-byte aligned */
struct metadata{
    size_t size;
    size_t isfree;
};

static unsigned char memory[MEM_SIZE] __attribute__((aligned(8)));
static size_t frontier = 0;

void *afalloc(size_t size){
    if (size == 0 || size > MEM_SIZE - sizeof(struct metadata)) return NULL;
    size = (size + 7) & ~(size_t)7;

    if (MEM_SIZE - frontier < sizeof(struct metadata)) return NULL;
    if (size > MEM_SIZE - frontier - sizeof(struct metadata)) return NULL;

    struct metadata *header = (struct metadata *)&memory[frontier];
    header->size = size;
    header->isfree = 0;
    frontier += sizeof(struct metadata) + size;
    return (void*)(header + 1);
}

void afa_reset(void){
    frontier = 0;
}
