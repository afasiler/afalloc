#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#ifndef AFA_SIZE
#define AFA_SIZE ((size_t)1024 * 1024)
#endif
#define ARENA ((size_t)AFA_SIZE)
void *afalloc(size_t); void afa_reset(void);
int main(void){
  assert(!afalloc(0)); assert(!afalloc(SIZE_MAX)); assert(!afalloc(2 * ARENA));
  void*a=afalloc(1); assert(a && (uintptr_t)a%8==0);
  assert(!afalloc(ARENA)); assert(afalloc(8));
  afa_reset(); assert(afalloc(ARENA-16)); assert(!afalloc(1));
  puts("test_arena_rv1103: all tests passed"); return 0; }
