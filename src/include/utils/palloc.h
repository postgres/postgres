/*-------------------------------------------------------------------------
 *
 * palloc.h
 *	  POSTGRES memory allocator definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: palloc.h,v 1.10 1999/07/14 01:20:30 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PALLOC_H
#define PALLOC_H

#ifdef PALLOC_IS_MALLOC

#define palloc(s)	  malloc(s)
#define pfree(p)	  free(p)
#define repalloc(p,s) realloc((p),(s))

#else							/* ! PALLOC_IS_MALLOC */

/* ----------
 * In the case we use memory contexts, use macro's for palloc() etc.
 * ----------
 */
#define palloc(s)	  ((void *)MemoryContextAlloc(CurrentMemoryContext,(Size)(s)))
#define pfree(p)	  MemoryContextFree(CurrentMemoryContext,(Pointer)(p))
#define repalloc(p,s) ((void *)MemoryContextRealloc(CurrentMemoryContext,(Pointer)(p),(Size)(s)))

#endif	 /* PALLOC_IS_MALLOC */

/* like strdup except uses palloc */
extern char *pstrdup(char *pointer);

#endif	 /* PALLOC_H */
