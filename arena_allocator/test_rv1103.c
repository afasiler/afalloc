#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
void *afalloc(size_t); void afa_reset(void);
int main(void){
  assert(!afalloc(0)); assert(!afalloc(SIZE_MAX)); assert(!afalloc(2*1024*1024));
  void*a=afalloc(1); assert(a && (uintptr_t)a%8==0);
  assert(!afalloc(1024*1024)); assert(afalloc(8));
  afa_reset(); assert(afalloc(1024*1024-16)); assert(!afalloc(1));
  puts("test_arena_rv1103: all tests passed"); return 0; }
