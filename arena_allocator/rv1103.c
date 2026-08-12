/*
custom arena allocator for luckfox pico mini a rv1103
but it can work through all mcu and low level computer to get max performance with allocation.
*/


#include <stdio.h>
#include <stdlib.h>

#define MEM_SIZE (1024*1024)
unsigned char memory[MEM_SIZE];
struct metadata{
    int size;
    int isfree;
};

int frontier = 0;

void *afalloc(int size){
    if (size <= 0) return NULL; 
    size = (size + 7) & ~7;

    int cur_offset = (frontier == -1) ? 0 : frontier; 
    if((cur_offset + size + sizeof(struct metadata)) <= MEM_SIZE){
        if(frontier == -1){
            struct metadata *header = (struct metadata*)&memory;
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
    frontier = -1;
    struct metadata *header = (struct metadata*)memory;
    header->isfree = 1;
    header->size = MEM_SIZE - sizeof(struct metadata);
}
