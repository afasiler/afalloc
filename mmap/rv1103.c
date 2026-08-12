/*
custom arena allocator for luckfox pico mini a rv1103
mmap-backed version of arena_allocator/rv1103.c - same afalloc/afa_reset
logic, only difference is where the backing memory comes from.
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define MEM_SIZE (1024*1024)
unsigned char *memory = NULL; //ilk afalloc/afa_reset cagrisinda mmap ile alinir, static dizi degil

struct metadata{
    int size;
    int isfree;
};

int frontier = 0;

static int ensure_memory(void){
    if (memory != NULL) return 0;
    memory = (unsigned char*)mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        memory = NULL;
        return -1;
    }
    return 0;
}

void *afalloc(int size){
    if (size <= 0) return NULL; //0 ve negatif reddedilir, sessizce yuvarlanip gecmez
    if (ensure_memory() != 0) return NULL;
    size = (size + 7) & ~7;

    int cur_offset = (frontier == -1) ? 0 : frontier; //frontier==-1 sentinel, gercek kullanilan byte 0'dir
    if((cur_offset + size + sizeof(struct metadata)) <= MEM_SIZE){//ife giremiyorsa bi tane daha arena alanı çekeriz.
        if(frontier == -1){
            struct metadata *header = (struct metadata*)memory;
            header->isfree = 0;
            header->size = size;
            frontier = sizeof(struct metadata) + header->size;
            void *free_void = (void*)(header + 1);
            return free_void;
        }
        else{
            struct metadata *header = (struct metadata *)&memory[frontier];
            void *free_void = (void*)(header + 1);
            header->size = size;
            header->isfree = 0;
            frontier += sizeof(struct metadata) + size;
            return free_void;
        }
    }
    return NULL;
}

void afa_reset(){
    if (ensure_memory() != 0) return; //mmap hic yapilmadiysa yazacak yer yok
    frontier = -1;
    struct metadata *header = (struct metadata*)memory;
    header->isfree = 1;
    header->size = MEM_SIZE - sizeof(struct metadata);
}//her frame sonrası veya ihtiyac sonrası geri verilmesin sıfırlansın yeniden kullanılsın syscallarla sbrk brklarla mmaplerle uygrasmamalıyız.
