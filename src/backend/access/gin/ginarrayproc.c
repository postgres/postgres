/*-------------------------------------------------------------------------
 *
 * ginarrayproc.c
 *	  support functions for GIN's indexing of any array
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gin/ginarrayproc.c,v 1.7 2006/10/06 17:13:58 petere Exp $
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin.h"
#include "utils/array.h"
#include "utils/lsyscache.h"


#define GinOverlapStrategy		1
#define GinContainsStrategy		2
#define GinContainedStrategy	3
#define GinEqualStrategy		4

#define ARRAYCHECK(x) do {									\
	if ( ARR_HASNULL(x) )									\
		ereport(ERROR,										\
			(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),		\
			 errmsg("array must not contain null values")));		\
} while(0)


/*
 * Function used as extractValue and extractQuery both
 */
Datum
ginarrayextract(PG_FUNCTION_ARGS)
{
	ArrayType  *array;
	uint32	   *nentries = (uint32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = NULL;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;

	/*
	 * we should guarantee that array will not be destroyed during all
	 * operation
	 */
	array = PG_GETARG_ARRAYTYPE_P_COPY(0);

	ARRAYCHECK(array);

	get_typlenbyvalalign(ARR_ELEMTYPE(array),
						 &elmlen, &elmbyval, &elmalign);

	deconstruct_array(array,
					  ARR_ELEMTYPE(array),
					  elmlen, elmbyval, elmalign,
					  &entries, NULL, (int *) nentries);

	/* we should not free array, entries[i] points into it */
	PG_RETURN_POINTER(entries);
}

Datum
ginarrayconsistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);
	ArrayType  *query = PG_GETARG_ARRAYTYPE_P(2);
	int			res,
				i,
				nentries;

	/* ARRAYCHECK was already done by previous ginarrayextract call */

	switch (strategy)
	{
		case GinOverlapStrategy:
		case GinContainedStrategy:
			/* at least one element in check[] is true, so result = true */
			res = TRUE;
			break;
		case GinContainsStrategy:
		case GinEqualStrategy:
			nentries = ArrayGetNItems(ARR_NDIM(query), ARR_DIMS(query));
			res = TRUE;
			for (i = 0; i < nentries; i++)
				if (!check[i])
				{
					res = FALSE;
					break;
				}
			break;
		default:
			elog(ERROR, "ginarrayconsistent: unknown strategy number: %d",
				 strategy);
			res = FALSE;
	}

	PG_RETURN_BOOL(res);
}
