#ifndef __M68K_UNALIGNED_H
#define __M68K_UNALIGNED_H

#include <linux/config.h>

#ifdef CONFIG_COLDFIRE

/* Use memmove here, so gcc does not insert a __builtin_memcpy. */

#define get_unaligned(ptr) \
  ({ __typeof__(*(ptr)) __tmp; memmove(&__tmp, (ptr), sizeof(*(ptr))); __tmp; })

#define put_unaligned(val, ptr)				\
  ({ __typeof__(*(ptr)) __tmp = (val);			\
     memmove((ptr), &__tmp, sizeof(*(ptr)));		\
     (void)0; })

#else
/*
 * The m68k can do unaligned accesses itself. 
 *
 * The strange macros are there to make sure these can't
 * be misused in a way that makes them not work on other
 * architectures where unaligned accesses aren't as simple.
 */

#define get_unaligned(ptr) (*(ptr))
#define put_unaligned(val, ptr) ((void)( *(ptr) = (val) ))

#endif

#endif
