#include <stddef.h>
#include <sys/mman.h>

#define MEM_SIZE (1024*1024)
#define MAX_CHUNKS 10

struct metadata{
    int size;
    int isfree;
};

typedef struct {
    unsigned char *memory;
    int frontier;
} chunk_t;

static chunk_t chunks[MAX_CHUNKS];
static int chunk_count = 0;
static int current_chunk = 1;        /* scratch starts at chunk 1, chunk 0 reserved for persistent */
static int persistent_frontier = -1; /* separate frontier for chunk 0 */

static int ensure_chunk(int index){
    if (index < chunk_count) return 0;
    if (index >= MAX_CHUNKS) return -1;
    unsigned char *base = (unsigned char*)mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
                                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return -1;
    chunks[index].memory = base;
    chunks[index].frontier = -1;
    if (index >= chunk_count) chunk_count = index + 1;
    return 0;
}

static void *alloc_in_chunk(unsigned char *mem, int *frontier_ptr, int size){
    int cur_offset = (*frontier_ptr == -1) ? 0 : *frontier_ptr;
    if ((cur_offset + size + (int)sizeof(struct metadata)) > MEM_SIZE) return NULL;
    if (*frontier_ptr == -1) {
        struct metadata *header = (struct metadata*)mem;
        header->isfree = 0;
        header->size = size;
        *frontier_ptr = sizeof(struct metadata) + header->size;
        return (void*)(header + 1);
    } else {
        struct metadata *header = (struct metadata *)&mem[*frontier_ptr];
        header->size = size;
        header->isfree = 0;
        *frontier_ptr += sizeof(struct metadata) + size;
        return (void*)(header + 1);
    }
}

void *afalloc_persistent(int size){
    if (size <= 0) return NULL;
    if (ensure_chunk(0) != 0) return NULL;
    size = (size + 7) & ~7;
    return alloc_in_chunk(chunks[0].memory, &persistent_frontier, size);
}

void *afalloc(int size){
    if (size <= 0) return NULL;
    size = (size + 7) & ~7;

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
        chunks[i].frontier = -1;
    }
    current_chunk = 1;
}
