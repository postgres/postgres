/*-------------------------------------------------------------------------
 *
 * ginarrayproc.c
 *	  support functions for GIN's indexing of any array
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gin/ginarrayproc.c,v 1.16 2009/06/11 14:48:53 momjian Exp $
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
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
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

	if (*nentries == 0 && PG_NARGS() == 3)
	{
		switch (PG_GETARG_UINT16(2))	/* StrategyNumber */
		{
			case GinOverlapStrategy:
				*nentries = -1; /* nobody can be found */
				break;
			case GinContainsStrategy:
			case GinContainedStrategy:
			case GinEqualStrategy:
			default:			/* require fullscan: GIN can't find void
								 * arrays */
				break;
		}
	}

	/* we should not free array, entries[i] points into it */
	PG_RETURN_POINTER(entries);
}

Datum
ginqueryarrayextract(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall3(ginarrayextract,
										PG_GETARG_DATUM(0),
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(2)));
}

Datum
ginarrayconsistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);
	ArrayType  *query = PG_GETARG_ARRAYTYPE_P(2);

	/* int32	nkeys = PG_GETARG_INT32(3); */
	/* Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res;
	int			i,
				nentries;

	/* ARRAYCHECK was already done by previous ginarrayextract call */

	switch (strategy)
	{
		case GinOverlapStrategy:
			/* result is not lossy */
			*recheck = false;
			/* at least one element in check[] is true, so result = true */
			res = true;
			break;
		case GinContainedStrategy:
			/* we will need recheck */
			*recheck = true;
			/* at least one element in check[] is true, so result = true */
			res = true;
			break;
		case GinContainsStrategy:
			/* result is not lossy */
			*recheck = false;
			/* must have all elements in check[] true */
			nentries = ArrayGetNItems(ARR_NDIM(query), ARR_DIMS(query));
			res = true;
			for (i = 0; i < nentries; i++)
			{
				if (!check[i])
				{
					res = false;
					break;
				}
			}
			break;
		case GinEqualStrategy:
			/* we will need recheck */
			*recheck = true;
			/* must have all elements in check[] true */
			nentries = ArrayGetNItems(ARR_NDIM(query), ARR_DIMS(query));
			res = true;
			for (i = 0; i < nentries; i++)
			{
				if (!check[i])
				{
					res = false;
					break;
				}
			}
			break;
		default:
			elog(ERROR, "ginarrayconsistent: unknown strategy number: %d",
				 strategy);
			res = false;
	}

	PG_RETURN_BOOL(res);
}
