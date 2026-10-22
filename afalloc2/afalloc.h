/* afalloc2: a general-purpose allocator for 64-bit POSIX systems (macOS, Linux), built for speed and
 * for hardening against heap memory-safety bugs. See README.md for the design and the measurements.
 *
 *   void *afalloc(size_t n);            at least n bytes, 16-byte aligned, NULL on failure
 *   void  afree(void *p);               NULL is ignored
 *   void *afcalloc(size_t n, size_t m);  zeroed, with an overflow check
 *   void *afrealloc(void *p, size_t n);
 *   void *afaligned_alloc(size_t align, size_t n);   align must be a power of two
 *   size_t afusable_size(const void *p); bytes you may use in the block (0 for a pointer it does not own)
 *
 * Security level (environment variable AFALLOC_SECURE, or afset_security() before the first allocation):
 *   0  fast:       no checks beyond the pointer being inside the heap
 *   1  hardened:   (default) free-list links are encrypted per slab, every free is validated (block start,
 *                  slab kind, immediate double free, wild free-list pointers), guard pages end every
 *                  slab and surround every large block
 *   2  paranoid:   level 1 plus an exact allocation bitmap (every double and invalid free is caught),
 *                  blocks up to 1 KB are zeroed on free, a delayed-reuse quarantine whose entries are
 *                  checked for writes-after-free, and randomised placement inside slabs
 * Corruption that is detected prints a message on stderr and aborts the process.
 */
#ifndef AFALLOC_H
#define AFALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *afalloc(size_t n);
void afree(void *p);
void *afcalloc(size_t n, size_t m);
void *afrealloc(void *p, size_t n);
void *afaligned_alloc(size_t align, size_t n);
size_t afusable_size(const void *p);

/* Choose the security level (0, 1 or 2). Only valid before the first allocation; returns 0 on success. */
int afset_security(int level);

typedef struct {
    size_t heap_reserved;       /* bytes of address space reserved for small blocks */
    size_t slabs_in_use;        /* 256 KiB slabs currently serving blocks */
    size_t slabs_committed;     /* slabs ever made accessible (high-water mark) */
    size_t large_live;          /* large (above 32 KiB) blocks currently allocated */
    size_t large_bytes;         /* bytes mapped for them */
    size_t slabs_abandoned;     /* slabs left by exited threads that still hold live blocks */
    int security_level;
} afstats_t;
void afget_stats(afstats_t *out);

/* Verify the allocator's internal bookkeeping (slab accounting, the lists of abandoned slabs). Returns 0 if
   consistent, a nonzero code otherwise. For tests and debugging; takes the global lock. */
int afcheck(void);

#ifdef __cplusplus
}
#endif

#endif
