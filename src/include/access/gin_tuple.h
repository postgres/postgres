/*--------------------------------------------------------------------------
 * gin.h
 *	  Public header file for Generalized Inverted Index access method.
 *
 *	Copyright (c) 2006-2025, PostgreSQL Global Development Group
 *
 *	src/include/access/gin.h
 *--------------------------------------------------------------------------
 */
#ifndef GIN_TUPLE_H
#define GIN_TUPLE_H

#include "access/ginblock.h"
#include "storage/itemptr.h"
#include "utils/sortsupport.h"

/*
 * Data for one key in a GIN index.
 */
typedef struct GinTuple
{
	int			tuplen;			/* length of the whole tuple */
	OffsetNumber attrnum;		/* attnum of index key */

	/*
	 * The key value is accessed in place, so data (below) must be aligned
	 * well enough for any key type.  We include both "double" and "int64" in
	 * the union to ensure that the compiler knows it must be MAXALIGN'ed (cf.
	 * configure's computation of MAXIMUM_ALIGNOF); this puts data at a
	 * MAXALIGN'ed offset, which the static assertion below verifies.  On
	 * 64-bit platforms, this already happens anyway; this trick is only
	 * needed on some 32-bit platforms.  Note that putting a flexible array
	 * member into a union is not valid C, so we have to pick some other
	 * member to move the alignment around.  This one just happens to be a
	 * space-efficient one.
	 */
	union
	{
		Size		keylen;		/* bytes in data for key value */
		double		force_align_d;
		int64		force_align_i64;
	}			u;
	int16		typlen;			/* typlen for key */
	bool		typbyval;		/* typbyval for key */
	signed char category;		/* category: normal or NULL? */
	int			nitems;			/* number of TIDs in the data */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} GinTuple;

StaticAssertDecl(offsetof(GinTuple, data) % MAXIMUM_ALIGNOF == 0,
				 "GinTuple.data must be MAXALIGN'ed");

static inline ItemPointer
GinTupleGetFirst(GinTuple *tup)
{
	GinPostingList *list;

	list = (GinPostingList *) SHORTALIGN(tup->data + tup->u.keylen);

	return &list->first;
}

extern int	_gin_compare_tuples(GinTuple *a, GinTuple *b, SortSupport ssup);

#endif							/* GIN_TUPLE_H */
