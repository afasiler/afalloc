/* malloc() obtains memory from _sbrk(). Hand it everything between the end of the pool
 * region and the stack reserve (symbols from linker.ld). Only the malloc benchmark build
 * uses this file. */
#include <errno.h>
#include <stddef.h>

extern char _sheap, _eheap;

void *_sbrk(ptrdiff_t increment)
{
    static char *brk;
    if (brk == 0)
        brk = &_sheap;
    if (increment < 0 || brk + increment > &_eheap) {
        errno = ENOMEM;
        return (void *)-1;
    }
    char *previous = brk;
    brk += increment;
    return previous;
}
