#include <stdio.h>
#include <stddef.h>
#include "arena_malloc.h"

#define MEM_SIZE ((size_t)1024 * 1024)
static unsigned char memory[MEM_SIZE] __attribute__((aligned(8)));

/* Both structs are a multiple of 8 bytes on 32- and 64-bit targets so every
   header and user pointer stays 8-byte aligned. */
struct metadata{
    size_t size;
    size_t isfree;
};

struct footer{
    struct metadata *header;
    size_t reserved;
};

static size_t frontier = 0;

void arena_coalesce(struct metadata *header){
    if((unsigned char *)header > memory){
        struct footer *prev_footer = (struct footer *)((unsigned char *)header - sizeof(struct footer));
        if(prev_footer->header->isfree == 1){
            struct metadata *prev_header = prev_footer->header;
            prev_header->size += sizeof(struct footer) + sizeof(struct metadata) + header->size;
            struct footer *current_footer = (struct footer *)((unsigned char *)prev_header + sizeof(struct metadata) + prev_header->size);
            current_footer->header = prev_header;
            header = prev_header;
        }
    }
    struct metadata *next_header = (struct metadata *)((unsigned char *)header + sizeof(struct metadata) + header->size + sizeof(struct footer));
    if((unsigned char *)next_header < (memory + frontier) && next_header->isfree == 1){
        header->size += sizeof(struct footer) + sizeof(struct metadata) + next_header->size;
        struct footer *next_footer = (struct footer *)((unsigned char *)header + sizeof(struct metadata) + header->size);
        next_footer->header = header;
    }
}

/* takes the pointer returned by afalloc(), like free() */
void arena_free(void *ptr){
    if(ptr == NULL){
        return;
    }
    struct metadata *head = (struct metadata *)ptr - 1;
    if(head->isfree == 1){
        return;
    }
    head->isfree = 1;
    arena_coalesce(head);
}

void *afalloc(size_t size){
    if(size == 0 || size > MEM_SIZE){
        return NULL;
    }
    size = (size + 7) & ~(size_t)7;   /* align first, then check the capacity */
    size_t overhead = sizeof(struct metadata) + sizeof(struct footer);
    if(MEM_SIZE - frontier < overhead || size > MEM_SIZE - frontier - overhead){
        return NULL;
    }
    struct metadata *header = (struct metadata *)&memory[frontier];
    header->size = size;
    header->isfree = 0;
    frontier += sizeof(struct metadata) + size;
    struct footer *pheader = (struct footer *)&memory[frontier];
    pheader->header = header;
    frontier += sizeof(struct footer);
    return (void*)(header + 1);
}

void afa_reset(void){
    frontier = 0;
}

#ifdef ARENA_DEMO
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
#endif
// char dizisi olarak kullan.
