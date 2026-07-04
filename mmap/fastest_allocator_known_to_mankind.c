#include <sys/mman.h>
#include <stddef.h>
#include <stdint.h>

unsigned char *mem = NULL;
int frontier = 0;

struct metadata {
    size_t size;
    uint32_t magic;
    int free;
};

void* chunk() {
    return mmap(NULL, 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

void f_coalescing() {
    if (mem == NULL || frontier == 0) return;
    
    int curr = 0;
    while (curr < frontier) {
        struct metadata *isIt = (struct metadata*)(mem + curr);
        
        if (isIt->magic != 0xAFA2BABA) break;

        if (isIt->free == 1) {
            int next = curr + sizeof(struct metadata) + isIt->size;
            
            if (next < frontier) {
                struct metadata *nextIt = (struct metadata*)(mem + next);
                
                if (nextIt->magic == 0xAFA2BABA && nextIt->free == 1) {
                    isIt->size += sizeof(struct metadata) + nextIt->size;
                    continue; 
                }
            }
        }
        curr += sizeof(struct metadata) + isIt->size;
    }
}

void* afalloc(size_t size) {
    if (mem == NULL) {
        mem = (unsigned char*)chunk();
    }
    
    size = (size + 7) & ~7; 
    int indexmaxxing = 0;
    
    while (indexmaxxing < 1024 * 1024) {
        if (indexmaxxing == frontier) {
            struct metadata *head = (struct metadata*)(mem + frontier);
            head->size = size;
            head->magic = 0xAFA2BABA;
            head->free = 0;
            
            frontier += sizeof(struct metadata) + size;
            return (void*)((unsigned char*)head + sizeof(struct metadata));
        } else {
            struct metadata *isIt = (struct metadata*)(mem + indexmaxxing);
            
            if (isIt->magic == 0xAFA2BABA) {
                if (isIt->free == 1 && isIt->size >= size) {
                    if (isIt->size >= size + sizeof(struct metadata) + 8) {
                        struct metadata *yeni_blok = (struct metadata*)((unsigned char*)isIt + sizeof(struct metadata) + size);
                        yeni_blok->size = isIt->size - size - sizeof(struct metadata);
                        yeni_blok->magic = 0xAFA2BABA;
                        yeni_blok->free = 1;
                        
                        isIt->size = size;
                    }
                    isIt->free = 0;
                    return (void*)((unsigned char*)isIt + sizeof(struct metadata));
                } else {
                    indexmaxxing += sizeof(struct metadata) + isIt->size;
                }
            } else {
                return NULL;
            }
        }
    }
    return NULL;
}

void f_free(void *ptr) {
    if (ptr == NULL) return;

    struct metadata *head = (struct metadata*)((unsigned char*)ptr - sizeof(struct metadata));
    if (head->magic == 0xAFA2BABA) {
        head->free = 1;
        f_coalescing();
    }
}

void nuke() {
    if (mem == NULL) return;
    
    struct metadata *start = (struct metadata*)mem;
    start->size = (1024 * 1024) - sizeof(struct metadata);
    start->magic = 0xAFA2BABA;
    start->free = 1;
    frontier = 1024 * 1024;
}