#include <stddef.h>
#include <sys/mman.h>

#define MEM_SIZE (1024*1024)
#define MAX_CHUNKS 32

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
static int current_chunk = 0;

static int ensure_chunk(int index){
    if (index < chunk_count) return 0;
    if (index >= MAX_CHUNKS) return -1;
    unsigned char *base = (unsigned char*)mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
                                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return -1;
    chunks[index].memory = base;
    chunks[index].frontier = -1;
    chunk_count = index + 1;
    return 0;
}

void *afalloc(int size){
    if (size <= 0) return NULL;
    size = (size + 7) & ~7;

    while (current_chunk < MAX_CHUNKS) {
        if (ensure_chunk(current_chunk) != 0) return NULL;
        chunk_t *c = &chunks[current_chunk];
        int cur_offset = (c->frontier == -1) ? 0 : c->frontier;

        if ((cur_offset + size + (int)sizeof(struct metadata)) <= MEM_SIZE) {
            if (c->frontier == -1) {
                struct metadata *header = (struct metadata*)c->memory;
                header->isfree = 0;
                header->size = size;
                c->frontier = sizeof(struct metadata) + header->size;
                return (void*)(header + 1);
            } else {
                struct metadata *header = (struct metadata *)&c->memory[c->frontier];
                header->size = size;
                header->isfree = 0;
                c->frontier += sizeof(struct metadata) + size;
                return (void*)(header + 1);
            }
        }
        current_chunk++;
    }
    return NULL;
}

void afa_reset(void){
    for (int i = 0; i < chunk_count; i++) {
        chunks[i].frontier = -1;
    }
    current_chunk = 0;
}
