/*--------------------------------------------------------------------------
 * gin.h
 *	  Public header file for Generalized Inverted Index access method.
 *
 *	Copyright (c) 2006-2024, PostgreSQL Global Development Group
 *
 *	src/include/access/gin.h
 *--------------------------------------------------------------------------
 */
#ifndef GIN_TUPLE_
#define GIN_TUPLE_

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
	uint16		keylen;			/* bytes in data for key value */
	int16		typlen;			/* typlen for key */
	bool		typbyval;		/* typbyval for key */
	signed char category;		/* category: normal or NULL? */
	int			nitems;			/* number of TIDs in the data */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} GinTuple;

static inline ItemPointer
GinTupleGetFirst(GinTuple *tup)
{
	GinPostingList *list;

	list = (GinPostingList *) SHORTALIGN(tup->data + tup->keylen);

	return &list->first;
}

extern int	_gin_compare_tuples(GinTuple *a, GinTuple *b, SortSupport ssup);

#endif							/* GIN_TUPLE_H */
