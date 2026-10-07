#ifndef AFALLOC_PERSISTENT_H
#define AFALLOC_PERSISTENT_H

#include <stddef.h>

void *afalloc(size_t size);            /* scratch region, cleared by afa_reset() */
void *afalloc_persistent(size_t size); /* persistent region, never cleared */
void  afa_reset(void);                 /* reset scratch chunks only */

#endif
