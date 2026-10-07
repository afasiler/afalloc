#ifndef AFALLOC_PERSISTENT_H
#define AFALLOC_PERSISTENT_H

#include <stddef.h>

/* Two independent pools, each up to AFA's MAX_CHUNKS 1 MiB chunks:
     scratch    - afalloc(),            cleared by afa_reset()
     persistent - afalloc_persistent(), never cleared by afa_reset()
   Every public function is thread-safe (a spinlock guards the allocator state)
   unless the library is built with -DAFA_NO_LOCK. */

/* Every block carries this many bytes of header in front of the user pointer;
   the largest request is the chunk size minus this. */
#define AFA_HEADER_SIZE 8

void *afalloc(size_t size);
void *afalloc_persistent(size_t size);

/* Return a block from either pool to its size class for reuse. O(1), no
   coalescing. Ignores NULL and blocks that are already free. Pointers must
   come from afalloc()/afalloc_persistent() and must not be freed after the
   pool they came from was reset or released. */
void  afree(void *ptr);

/* Rewind every scratch chunk and drop scratch free lists. Invalidates all
   scratch pointers. Chunks stay mapped for reuse. */
void  afa_reset(void);

/* afa_reset() plus munmap() of every scratch chunk, returning the memory to
   the OS. The next afalloc() maps again. */
void  afa_trim(void);

/* Map and fault in the first `scratch_chunks` and `persistent_chunks` chunks up
   front (at most 10 each) so later allocations never call mmap or take a page
   fault on first touch: deterministic latency for real-time use. Returns 0, or
   -1 if a mapping failed (chunks mapped so far stay mapped). */
int   afa_prefault(unsigned scratch_chunks, unsigned persistent_chunks);

/* Unmap both pools. Invalidates every pointer ever returned. */
void  afa_destroy(void);

#endif
