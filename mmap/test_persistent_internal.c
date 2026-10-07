#undef NDEBUG
/* White-box tests: include the implementation to reach its static helpers. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "afalloc_persistent.c"

/* every request size maps to a valid class whose block fits it, afree() (which
   only knows the class) lands in the same class, and waste stays under 25% */
static void test_size_classes_exhaustive(void){
    int prev = -1;
    size_t prev_rounded = 0;
    for (size_t s = 1; s <= MAX_REQUEST; s++) {
        size_t r;
        int i = size_class(s, &r);
        assert(i >= 0 && i < NUM_CLASSES);
        assert(r >= s && r <= MAX_REQUEST && r % 8 == 0);
        assert(class_size(i) == r);
        assert(i >= prev);
        if (i > prev) assert(r > prev_rounded);
        if (s > SMALL_LIMIT) assert((r - s) * 4 <= s);
        prev = i;
        prev_rounded = r;
    }
}

/* a request that does not fit the rest of a chunk opens the next chunk, but
   the abandoned tail becomes free blocks that same-class requests reuse */
static void test_tail_is_recycled(void){
    afa_destroy();
    size_t big = MAX_REQUEST / 2 + 4096;
    unsigned char *a = afalloc(big);
    assert(a != NULL);
    unsigned char *chunk0 = scratch_pool.chunks[0];
    size_t tail = scratch_pool.left;                 /* what chunk 0 has left */
    assert(tail >= HEADER_SIZE + 8 && tail < big);

    unsigned char *b = afalloc(big);                 /* does not fit: opens chunk 1 */
    assert(b != NULL && b >= scratch_pool.chunks[1] && b < scratch_pool.chunks[1] + MEM_SIZE);

    /* the first carved block is the largest class that fits the tail */
    size_t usable = tail - HEADER_SIZE, rounded;
    int cls = size_class(usable, &rounded);
    if (rounded > usable) { cls--; rounded = class_size(cls); }
    unsigned char *c = afalloc(rounded);
    assert(c != NULL && c >= chunk0 && c < chunk0 + MEM_SIZE);
    memset(c, 0x42, rounded);                        /* really inside the old chunk */
    afa_destroy();
}

/* afa_reset() clears every free list that was used */
static void test_free_lists_cleared_by_reset(void){
    afa_destroy();
    void *a = afalloc(100);
    afree(a);
    assert(scratch_pool.max_class >= 0);
    afa_reset();
    assert(scratch_pool.max_class == -1);
    for (int i = 0; i < NUM_CLASSES; i++) assert(scratch_pool.free_lists[i] == NULL);
    afa_destroy();
}

int main(void){
    test_size_classes_exhaustive();
    test_tail_is_recycled();
    test_free_lists_cleared_by_reset();
    puts("test_persistent_internal: all tests passed");
    return 0;
}
