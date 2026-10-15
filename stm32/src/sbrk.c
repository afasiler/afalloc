/* malloc() obtains memory from _sbrk(). Hand it everything between the end of the pool
 * region and the bottom of the stack (symbols from linker.ld). Only the malloc benchmark
 * build uses this file. */
#include <errno.h>
#include <stddef.h>

extern char _sheap, _eheap;

void *_sbrk(ptrdiff_t increment)
{
    static char *brk;
    if (brk == 0)
        brk = &_sheap;
    /* Compare sizes, not pointers: brk + increment could wrap before it is checked. nano
       malloc never gives memory back, so shrinking is refused. */
    if (increment < 0 || (size_t)increment > (size_t)(&_eheap - brk)) {
        errno = ENOMEM;
        return (void *)-1;
    }
    char *previous = brk;
    brk += increment;
    return previous;
}
