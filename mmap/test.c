#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mmap_allocator.h"

/* tests for the general-purpose allocator in mmap_allocator.c; they assume the
   16-byte header of a 64-bit target */

struct node{
    int number;
    struct node* next;
};
struct node2{
    size_t number;
    struct node* next;
};

#define REGION (1024 * 1024)
#define HEADER 16

static void test_zero_size(void){
    assert(afalloc(0) == NULL);        /* no block is created for size 0 */
    /* regression: (size + 7) & ~7 wrapped to 0 and returned a 0-byte block */
    assert(afalloc(SIZE_MAX) == NULL);
    assert(afalloc(SIZE_MAX - 3) == NULL);
    assert(afalloc(SIZE_MAX - 7) == NULL);
    assert(afalloc(REGION - HEADER + 1) == NULL);
    f_free(NULL);                      /* must be a harmless no-op */
    reset_region();
}

static void test_region_bounds(void){
    assert(afalloc(REGION) == NULL);
    assert(afalloc(REGION - HEADER + 1) == NULL);
    void *p = afalloc(REGION - HEADER);   /* exactly fills the region */
    assert(p != NULL);
    assert(afalloc(1) == NULL);
    f_free(p);
    reset_region();
}

static void test_fill_region(void){
    void *first = NULL;
    for (int i = 0; i < 1024; i++) {      /* 1024 * (1008 + 16 header) == 1 MiB */
        void *p = afalloc(1008);
        assert(p != NULL);
        if (i == 0) first = p;
    }
    assert(afalloc(1008) == NULL);
    reset_region();
    assert(afalloc(1008) == first);       /* reset starts over at the base */
    reset_region();
}

static void test_linked_list_and_reuse(void){
    struct node *first = afalloc(sizeof(struct node));
    struct node *second = afalloc(sizeof(struct node));
    struct node *third = afalloc(sizeof(struct node));
    assert(first && second && third);
    assert((uintptr_t)first % 8 == 0);
    assert((unsigned char*)second == (unsigned char*)first + sizeof(struct node) + HEADER);

    first->number = 15;  first->next = second;
    second->number = 30; second->next = third;
    third->number = 45;  third->next = NULL;
    assert(first->next->next->number == 45);

    f_free(first);
    f_free(second);                       /* adjacent free blocks coalesce */

    struct node2 *big = afalloc(sizeof(struct node2) + 16);
    assert((void*)big == (void*)first);   /* fits only in the merged block */
    big->number = 100;
    assert(third->number == 45);          /* neighbour untouched */
    reset_region();
}

static void test_free_rejects_foreign_pointer(void){
    unsigned char fake[64];
    memset(fake, 0, sizeof fake);
    f_free(fake + 32);                    /* no magic header -> ignored */
}

int main(void){
    test_zero_size();
    test_region_bounds();
    test_fill_region();
    test_linked_list_and_reuse();
    test_free_rejects_foreign_pointer();
    puts("test_mmap: all tests passed");
    return 0;
}
