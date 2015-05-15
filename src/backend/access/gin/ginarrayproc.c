/*-------------------------------------------------------------------------
 *
 * ginarrayproc.c
 *	  support functions for GIN's indexing of any array
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gin/ginarrayproc.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/stratnum.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


#define GinOverlapStrategy		1
#define GinContainsStrategy		2
#define GinContainedStrategy	3
#define GinEqualStrategy		4


/*
 * extractValue support function
 */
Datum
ginarrayextract(PG_FUNCTION_ARGS)
{
	/* Make copy of array input to ensure it doesn't disappear while in use */
	ArrayType  *array = PG_GETARG_ARRAYTYPE_P_COPY(0);
	int32	   *nkeys = (int32 *) PG_GETARG_POINTER(1);
	bool	  **nullFlags = (bool **) PG_GETARG_POINTER(2);
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;

	get_typlenbyvalalign(ARR_ELEMTYPE(array),
						 &elmlen, &elmbyval, &elmalign);

	deconstruct_array(array,
					  ARR_ELEMTYPE(array),
					  elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	*nkeys = nelems;
	*nullFlags = nulls;

	/* we should not free array, elems[i] points into it */
	PG_RETURN_POINTER(elems);
}

/*
 * Formerly, ginarrayextract had only two arguments.  Now it has three,
 * but we still need a pg_proc entry with two args to support reloading
 * pre-9.1 contrib/intarray opclass declarations.  This compatibility
 * function should go away eventually.
 */
Datum
ginarrayextract_2args(PG_FUNCTION_ARGS)
{
	if (PG_NARGS() < 3)			/* should not happen */
		elog(ERROR, "ginarrayextract requires three arguments");
	return ginarrayextract(fcinfo);
}

/*
 * extractQuery support function
 */
Datum
ginqueryarrayextract(PG_FUNCTION_ARGS)
{
	/* Make copy of array input to ensure it doesn't disappear while in use */
	ArrayType  *array = PG_GETARG_ARRAYTYPE_P_COPY(0);
	int32	   *nkeys = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);

	/* bool   **pmatch = (bool **) PG_GETARG_POINTER(3); */
	/* Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	bool	  **nullFlags = (bool **) PG_GETARG_POINTER(5);
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;

	get_typlenbyvalalign(ARR_ELEMTYPE(array),
						 &elmlen, &elmbyval, &elmalign);

	deconstruct_array(array,
					  ARR_ELEMTYPE(array),
					  elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	*nkeys = nelems;
	*nullFlags = nulls;

	switch (strategy)
	{
		case GinOverlapStrategy:
			*searchMode = GIN_SEARCH_MODE_DEFAULT;
			break;
		case GinContainsStrategy:
			if (nelems > 0)
				*searchMode = GIN_SEARCH_MODE_DEFAULT;
			else	/* everything contains the empty set */
				*searchMode = GIN_SEARCH_MODE_ALL;
			break;
		case GinContainedStrategy:
			/* empty set is contained in everything */
			*searchMode = GIN_SEARCH_MODE_INCLUDE_EMPTY;
			break;
		case GinEqualStrategy:
			if (nelems > 0)
				*searchMode = GIN_SEARCH_MODE_DEFAULT;
			else
				*searchMode = GIN_SEARCH_MODE_INCLUDE_EMPTY;
			break;
		default:
			elog(ERROR, "ginqueryarrayextract: unknown strategy number: %d",
				 strategy);
	}

	/* we should not free array, elems[i] points into it */
	PG_RETURN_POINTER(elems);
}

/*
 * consistent support function
 */
Datum
ginarrayconsistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* ArrayType  *query = PG_GETARG_ARRAYTYPE_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);

	/* Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);

	/* Datum	   *queryKeys = (Datum *) PG_GETARG_POINTER(6); */
	bool	   *nullFlags = (bool *) PG_GETARG_POINTER(7);
	bool		res;
	int32		i;

	switch (strategy)
	{
		case GinOverlapStrategy:
			/* result is not lossy */
			*recheck = false;
			/* must have a match for at least one non-null element */
			res = false;
			for (i = 0; i < nkeys; i++)
			{
				if (check[i] && !nullFlags[i])
				{
					res = true;
					break;
				}
			}
			break;
		case GinContainsStrategy:
			/* result is not lossy */
			*recheck = false;
			/* must have all elements in check[] true, and no nulls */
			res = true;
			for (i = 0; i < nkeys; i++)
			{
				if (!check[i] || nullFlags[i])
				{
					res = false;
					break;
				}
			}
			break;
		case GinContainedStrategy:
			/* we will need recheck */
			*recheck = true;
			/* can't do anything else useful here */
			res = true;
			break;
		case GinEqualStrategy:
			/* we will need recheck */
			*recheck = true;

			/*
			 * Must have all elements in check[] true; no discrimination
			 * against nulls here.  This is because array_contain_compare and
			 * array_eq handle nulls differently ...
			 */
			res = true;
			for (i = 0; i < nkeys; i++)
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

/*
 * triconsistent support function
 */
Datum
ginarraytriconsistent(PG_FUNCTION_ARGS)
{
	GinTernaryValue *check = (GinTernaryValue *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* ArrayType  *query = PG_GETARG_ARRAYTYPE_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);

	/* Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	/* Datum	   *queryKeys = (Datum *) PG_GETARG_POINTER(5); */
	bool	   *nullFlags = (bool *) PG_GETARG_POINTER(6);
	GinTernaryValue res;
	int32		i;

	switch (strategy)
	{
		case GinOverlapStrategy:
			/* must have a match for at least one non-null element */
			res = GIN_FALSE;
			for (i = 0; i < nkeys; i++)
			{
				if (!nullFlags[i])
				{
					if (check[i] == GIN_TRUE)
					{
						res = GIN_TRUE;
						break;
					}
					else if (check[i] == GIN_MAYBE && res == GIN_FALSE)
					{
						res = GIN_MAYBE;
					}
				}
			}
			break;
		case GinContainsStrategy:
			/* must have all elements in check[] true, and no nulls */
			res = GIN_TRUE;
			for (i = 0; i < nkeys; i++)
			{
				if (check[i] == GIN_FALSE || nullFlags[i])
				{
					res = GIN_FALSE;
					break;
				}
				if (check[i] == GIN_MAYBE)
				{
					res = GIN_MAYBE;
				}
			}
			break;
		case GinContainedStrategy:
			/* can't do anything else useful here */
			res = GIN_MAYBE;
			break;
		case GinEqualStrategy:

			/*
			 * Must have all elements in check[] true; no discrimination
			 * against nulls here.  This is because array_contain_compare and
			 * array_eq handle nulls differently ...
			 */
			res = GIN_MAYBE;
			for (i = 0; i < nkeys; i++)
			{
				if (check[i] == GIN_FALSE)
				{
					res = GIN_FALSE;
					break;
				}
			}
			break;
		default:
			elog(ERROR, "ginarrayconsistent: unknown strategy number: %d",
				 strategy);
			res = false;
	}

	PG_RETURN_GIN_TERNARY_VALUE(res);
}
