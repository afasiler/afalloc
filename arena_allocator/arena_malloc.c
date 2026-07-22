#include <stdio.h>
#include <stdlib.h>

#define MEM_SIZE (1024*1024)
unsigned char memory[MEM_SIZE];

struct metadata{
    int size;
    int isfree;
};

struct footer{
    struct metadata *header;
};
int frontier = 0;
void arena_free(struct metadata *head){
    if(head == NULL){
        printf("its already null");
    }
    else{
        head->isfree = 1;
    }
}

void *afalloc(int size){
    if(frontier + size + sizeof(struct metadata)+ sizeof(struct footer)<= MEM_SIZE){
        int mod = size % 8;
        if(mod != 0){
            size = size +(8-mod);
        }
        struct metadata *header = (struct metadata *)&memory[frontier];
        void *free_void = (void*)(header + 1);
        header->size = size;
        header->isfree = 0;
        frontier += sizeof(struct metadata) + size;
        struct footer *pheader = (struct footer *)&memory[frontier];
        frontier += sizeof(struct footer);
        pheader->header = header;
        return free_void;
        
    }
    return NULL;
}

void arena_coalesce(struct metadata *header){
    struct footer *prev_footer = (struct footer *)((unsigned char *)header - sizeof(struct footer));
    if((unsigned char *)prev_footer >= memory && prev_footer->header->isfree == 1){
        struct metadata *prev_header = prev_footer->header;
        prev_header->size += sizeof(struct footer) + sizeof(struct metadata) + header->size;
        struct footer *current_footer = (struct footer *)((unsigned char *)prev_header + sizeof(struct metadata) + prev_header->size);
        current_footer->header = prev_header;
        header = prev_header;
    }
    struct metadata *next_header = (struct metadata *)((unsigned char *)header + sizeof(struct metadata) + header->size + sizeof(struct footer));
    if((unsigned char *)next_header < (memory + frontier) && next_header->isfree == 1){
        header->size += sizeof(struct footer) + sizeof(struct metadata) + next_header->size;
        struct footer *next_footer = (struct footer *)((unsigned char *)header + sizeof(struct metadata) + header->size);
        next_footer->header = header;
    }
}

void afa_reset(){
    frontier = 0;
}

int main(void){
    void *ptr1 = afalloc(16);
    printf("\nAllocated 16 bytes at %p", ptr1);
    void *ptr2 = afalloc(32);
    printf("\nAllocated 32 bytes at %p", ptr2);
    printf("\n%p", ptr1);
    printf("\n%p", ptr2);
    afa_reset();
    printf("\n%p", ptr1);
    printf("\n%p", ptr2);
    return 0;
}
// char dizisi olarak kullan.
