#include <sys/mman.h>
#include <stddef.h>
#include <stdint.h>
#include "mmap_allocator.h"


static unsigned char *mem = NULL;
static size_t frontier = 0;

struct metadata {
    size_t size;
    uint32_t magic;
    int free;
};

/* Chunk/region size in bytes; build with -DAFA_SIZE=<bytes> to change it. */
#ifndef AFA_SIZE
#define AFA_SIZE ((size_t)1024 * 1024)
#endif
#define REGION_SIZE ((size_t)AFA_SIZE)
#define MAX_REQUEST (REGION_SIZE - sizeof(struct metadata))

/* Offset of a block boundary at or before every free block (== frontier when
   nothing is free). Every block before it is in use, so searches and
   coalescing start here instead of at 0, which keeps allocating at the
   frontier O(1) instead of walking the whole list. */
static size_t lowest_free = 0;

static void* chunk(void) {
    return mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/* Merge adjacent free blocks, walking from lowest_free until the walk has
   passed `stop` (a block offset). Merging only ever looks forward, so a freed
   block is joined to a free predecessor when the walk reaches that predecessor. */
static void coalesce_until(size_t stop) {
    size_t curr = lowest_free;
    while (curr < frontier && curr <= stop) {
        struct metadata *isIt = (struct metadata*)(mem + curr);

        if (isIt->magic != 0xAFA2BABA) break;

        if (isIt->free == 1) {
            size_t next = curr + sizeof(struct metadata) + isIt->size;

            if (next < frontier) {
                struct metadata *nextIt = (struct metadata*)(mem + next);

                if (nextIt->magic == 0xAFA2BABA && nextIt->free == 1) {
                    isIt->size += sizeof(struct metadata) + nextIt->size;
                    nextIt->magic = 0;      /* absorbed: stale pointers to it must not pass afree */
                    continue;
                }
            }
        }
        curr += sizeof(struct metadata) + isIt->size;
    }
}

void afa_coalesce(void) {
    if (mem == NULL || frontier == 0) return;
    coalesce_until(frontier);
}

void* afalloc(size_t size) {
    /* reject before rounding: (size + 7) wraps to 0 for sizes near SIZE_MAX */
    if (size == 0 || size > MAX_REQUEST) return NULL;
    size = (size + 7) & ~(size_t)7;
    if (size > MAX_REQUEST) return NULL;

    if (mem == NULL) {
        void *base = chunk();
        if (base == MAP_FAILED) return NULL;
        mem = (unsigned char*)base;
    }
    size_t curr_index = lowest_free;
    int seen_free = 0;      /* a free block was passed, so lowest_free must stay put */

    while (curr_index < REGION_SIZE) {
        if (curr_index == frontier) {
            if (frontier + sizeof(struct metadata) + size > REGION_SIZE) {
                return NULL;
            }
            struct metadata *head = (struct metadata*)(mem + frontier);
            head->size = size;
            head->magic = 0xAFA2BABA;
            head->free = 0;

            frontier += sizeof(struct metadata) + size;
            if (!seen_free) lowest_free = frontier;
            return (void*)((unsigned char*)head + sizeof(struct metadata));
        } else {
            struct metadata *isIt = (struct metadata*)(mem + curr_index);

            if (isIt->magic == 0xAFA2BABA) {
                if (isIt->free == 1 && isIt->size >= size) {
                    size_t after = curr_index + sizeof(struct metadata) + size;
                    if (isIt->size >= size + sizeof(struct metadata) + 8) {
                        struct metadata *yeni_blok = (struct metadata*)((unsigned char*)isIt + sizeof(struct metadata) + size);
                        yeni_blok->size = isIt->size - size - sizeof(struct metadata);
                        yeni_blok->magic = 0xAFA2BABA;
                        yeni_blok->free = 1;

                        isIt->size = size;
                    } else {
                        after = curr_index + sizeof(struct metadata) + isIt->size;
                    }
                    isIt->free = 0;
                    /* the first free block was just used: the remainder (if
                       any) starts at `after`, and everything before is in use */
                    if (!seen_free) lowest_free = after;
                    return (void*)((unsigned char*)isIt + sizeof(struct metadata));
                } else {
                    if (isIt->free == 1) seen_free = 1;
                    curr_index += sizeof(struct metadata) + isIt->size;
                    if (!seen_free) lowest_free = curr_index;
                }
            } else {
                return NULL;
            }
        }
    }
    return NULL;
}

void afree(void *ptr) {
    if (ptr == NULL || mem == NULL) return;

    /* Only a block that starts inside the allocated part of the region can be
       real: a pointer from before afa_reset() or from outside the region
       is ignored without being dereferenced, because lowest_free must always
       stay a genuine block boundary. */
    unsigned char *p = (unsigned char*)ptr;
    if (p < mem + sizeof(struct metadata) || p >= mem + frontier) return;
    if (((size_t)(p - mem) & 7) != 0) return;

    struct metadata *head = (struct metadata*)(p - sizeof(struct metadata));
    if (head->magic == 0xAFA2BABA) {
        size_t offset = (size_t)((unsigned char*)head - mem);
        head->free = 1;
        if (offset < lowest_free) lowest_free = offset;
        coalesce_until(offset);
    }
}

void afa_reset(void) {
    if (mem == NULL) return;

    struct metadata *start = (struct metadata*)mem;
    start->size = REGION_SIZE - sizeof(struct metadata);
    start->magic = 0xAFA2BABA;
    start->free = 1;
    frontier = 0;
    lowest_free = 0;
}
