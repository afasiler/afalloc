#include <stdio.h>
#include "mmap_allocator.h"
struct node{
    int number;
    struct node* next;
    //12 bayt
};
struct node2{
    size_t number;
    struct node* next;
    // 16 bayt
};
void* pntr;
int main(void){

    pntr = afalloc(0); //expected to only create metada with size 0 || terminal output: 0x100b30010 
    printf("\n%p", pntr);
    f_free(pntr);
    reset_region();

    pntr = afalloc(1024*1024-16); //expected to fail because 16 bayts already occupied || terminal output: 0x100b30020 but must be 0x0 null || fixed by adding a new boundary constraint. 
    printf("\n%p", pntr);
    f_free(pntr);

    for (int i = 0; i < 1024; i++) { // filling up memory
        pntr = afalloc(1008); // header is 16 bayt and i choose the equal size of void pointer to be 1008. this way there shouldnt be any fragmentation. 
        printf("\n%d\t%p", i,pntr);         
    }
    pntr = afalloc(1008); //expected to be null || terminal output: 0x0 == null
    printf("\n%p", pntr); 
    
    f_free(pntr);
    reset_region();

    struct node* first = (struct node*)afalloc(sizeof(struct node));
    first->number = 15;

    struct node* second = (struct node*)afalloc(sizeof(struct node));
    first->next = second;
    second->number = 30;

    struct node* third = (struct node*)afalloc(sizeof(struct node));
    second->next = third;
    third->number = 45;
    third->next = NULL;

    printf("\n%p", first); // terminal output 0x100b30010
    printf("\n%d", first->number);
    printf("\n%p", second); //terminal output 0x100b30030
    printf("\n%d", second->number);
    printf("\n%p", third); //terminal output 0x100b30050
    printf("\n%d", third->number);

    f_free(first);
    f_free(second);

    struct node2* first2 = (struct node2*)afalloc(sizeof(struct node2));
    first2->number = 100;
    first2->next = third;
    printf("\n%p", first2);//terminal output 0x100b30010
    printf("\n%zu", first2->number);
    printf("\n%p", third); //terminal output 0x100b30050
    printf("\n%d", third->number);
}
