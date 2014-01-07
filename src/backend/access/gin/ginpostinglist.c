/*-------------------------------------------------------------------------
 *
 * ginpostinglist.c
 *	  routines for dealing with posting lists.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginpostinglist.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"

/*
 * Merge two ordered arrays of itempointers, eliminating any duplicates.
 * Returns the number of items in the result.
 * Caller is responsible that there is enough space at *dst.
 */
uint32
ginMergeItemPointers(ItemPointerData *dst,
					 ItemPointerData *a, uint32 na,
					 ItemPointerData *b, uint32 nb)
{
	ItemPointerData *dptr = dst;
	ItemPointerData *aptr = a,
			   *bptr = b;

	while (aptr - a < na && bptr - b < nb)
	{
		int			cmp = ginCompareItemPointers(aptr, bptr);

		if (cmp > 0)
			*dptr++ = *bptr++;
		else if (cmp == 0)
		{
			/* we want only one copy of the identical items */
			*dptr++ = *bptr++;
			aptr++;
		}
		else
			*dptr++ = *aptr++;
	}

	while (aptr - a < na)
		*dptr++ = *aptr++;

	while (bptr - b < nb)
		*dptr++ = *bptr++;

	return dptr - dst;
}
