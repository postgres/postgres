/*-------------------------------------------------------------------------
 *
 * palloc.c--
 *	  POSTGRES memory allocator code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/Attic/palloc.c,v 1.6 1998/02/26 04:38:22 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <string.h>

#include "c.h"

#include "utils/mcxt.h"
#include "utils/elog.h"
#include "utils/palloc.h"

#include "nodes/memnodes.h"

#include "utils/palloc.h"

/* ----------------------------------------------------------------
 *		User library functions
 * ----------------------------------------------------------------
 */

#undef palloc
#undef pfree
#undef MemoryContextAlloc
#undef MemoryContextFree
#undef malloc
#undef free

/* define PALLOC_IS_MALLOC if you want palloc to go straight to the
   raw malloc, without concern for the extra bookkeeping needed to
   ensure garbage is collected at the end of transactions - jolly 1/12/94 */


/*
 * palloc --
 *		Returns pointer to aligned memory of specified size.
 *
 * Exceptions:
 *		BadArgument if size < 1 or size >= MaxAllocSize.
 *		ExhaustedMemory if allocation fails.
 *		NonallocatedPointer if pointer was not returned by palloc or repalloc
 *				or may have been freed already.
 *
 * pfree --
 *		Frees memory associated with pointer returned from palloc or repalloc.
 *
 * Exceptions:
 *		BadArgument if pointer is invalid.
 *		FreeInWrongContext if pointer was allocated in a different "context."
 *		NonallocatedPointer if pointer was not returned by palloc or repalloc
 *				or may have been subsequently freed.
 */
void *
palloc(Size size)
{
#ifdef PALLOC_IS_MALLOC
	return malloc(size);
#else
	return (MemoryContextAlloc(CurrentMemoryContext, size));
#endif							/* PALLOC_IS_MALLOC */
}

void
pfree(void *pointer)
{
#ifdef PALLOC_IS_MALLOC
	free(pointer);
#else
	MemoryContextFree(CurrentMemoryContext, pointer);
#endif							/* PALLOC_IS_MALLOC */
}

/*
 * repalloc --
 *		Returns pointer to aligned memory of specified size.
 *
 * Side effects:
 *		The returned memory is first filled with the contents of *pointer
 *		up to the minimum of size and psize(pointer).  Pointer is freed.
 *
 * Exceptions:
 *		BadArgument if pointer is invalid or size < 1 or size >= MaxAllocSize.
 *		ExhaustedMemory if allocation fails.
 *		NonallocatedPointer if pointer was not returned by palloc or repalloc
 *				or may have been freed already.
 */
void *
repalloc(void *pointer, Size size)
{
#ifdef PALLOC_IS_MALLOC
	return realloc(pointer, size);
#else
	return (MemoryContextRealloc(CurrentMemoryContext, pointer, size));
#endif
}

/* pstrdup
	allocates space for and copies a string
	just like strdup except it uses palloc instead of malloc */
char *
pstrdup(char *string)
{
	char	   *nstr;

	nstr = (char *) palloc(strlen(string) + 1);
	strcpy(nstr, string);

	return nstr;
}
