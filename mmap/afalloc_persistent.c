#include <stddef.h>
#include <sys/mman.h>
#include "afalloc_persistent.h"

#define MEM_SIZE ((size_t)1024 * 1024)
#define MAX_CHUNKS 10

/* Two size_t fields keep the header a multiple of 8 bytes on both 32-bit and
   64-bit targets, so user pointers stay 8-byte aligned. */
struct metadata{
    size_t size;
    size_t isfree;
};

#define HEADER_SIZE sizeof(struct metadata)
#define MAX_REQUEST (MEM_SIZE - HEADER_SIZE)

typedef struct {
    unsigned char *memory;
    size_t frontier;
} chunk_t;

static chunk_t chunks[MAX_CHUNKS];
static int chunk_count = 0;
static int current_chunk = 1;       /* scratch starts at chunk 1, chunk 0 reserved for persistent */
static size_t persistent_frontier = 0; /* separate frontier for chunk 0 */

static int ensure_chunk(int index){
    if (index >= MAX_CHUNKS) return -1;
    /* chunk 0 can be mapped after scratch chunks, so test the pointer rather
       than chunk_count */
    if (chunks[index].memory != NULL) return 0;
    unsigned char *base = (unsigned char*)mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
                                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return -1;
    chunks[index].memory = base;
    chunks[index].frontier = 0;
    if (index >= chunk_count) chunk_count = index + 1;
    return 0;
}

static void *alloc_in_chunk(unsigned char *mem, size_t *frontier_ptr, size_t size){
    if (MEM_SIZE - *frontier_ptr < HEADER_SIZE) return NULL;
    if (size > MEM_SIZE - *frontier_ptr - HEADER_SIZE) return NULL;
    struct metadata *header = (struct metadata *)&mem[*frontier_ptr];
    header->size = size;
    header->isfree = 0;
    *frontier_ptr += HEADER_SIZE + size;
    return (void*)(header + 1);
}

/* Rounds up to a multiple of 8. Returns 0 if the request can never fit in a
   chunk, so callers reject it before touching any allocator state. */
static size_t normalize(size_t size){
    if (size == 0 || size > MAX_REQUEST) return 0;
    size = (size + 7) & ~(size_t)7;
    return size > MAX_REQUEST ? 0 : size;
}

void *afalloc_persistent(size_t size){
    size = normalize(size);
    if (size == 0) return NULL;
    if (ensure_chunk(0) != 0) return NULL;
    return alloc_in_chunk(chunks[0].memory, &persistent_frontier, size);
}

void *afalloc(size_t size){
    size = normalize(size);
    if (size == 0) return NULL;

    while (current_chunk < MAX_CHUNKS) {
        if (ensure_chunk(current_chunk) != 0) return NULL;
        void *p = alloc_in_chunk(chunks[current_chunk].memory, &chunks[current_chunk].frontier, size);
        if (p) return p;
        current_chunk++;
    }
    return NULL;
}

void afa_reset(void){
    for (int i = 1; i < chunk_count; i++) {
        chunks[i].frontier = 0;
    }
    current_chunk = 1;
}
