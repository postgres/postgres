/*-------------------------------------------------------------------------
 *
 * palloc.h--
 *	  POSTGRES memory allocator definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: palloc.h,v 1.7 1999/02/06 16:50:34 wieck Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PALLOC_H
#define PALLOC_H

#include "c.h"

#ifdef PALLOC_IS_MALLOC

#  define palloc(s)		malloc(s)
#  define pfree(p)		free(p)
#  define repalloc(p,s)	realloc((p),(s))

#else /* ! PALLOC_IS_MALLOC */

/* ----------
 * In the case we use memory contexts, use macro's for palloc() etc.
 * ----------
 */
#  include "utils/mcxt.h"

#  define palloc(s)		((void *)MemoryContextAlloc(CurrentMemoryContext,(Size)(s)))
#  define pfree(p)		MemoryContextFree(CurrentMemoryContext,(Pointer)(p))
#  define repalloc(p,s)	((void *)MemoryContextRealloc(CurrentMemoryContext,(Pointer)(p),(Size)(s)))

#endif /* PALLOC_IS_MALLOC */

/* like strdup except uses palloc */
extern char *pstrdup(char *pointer);

#endif	 /* PALLOC_H */
