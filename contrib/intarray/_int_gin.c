/*
 * contrib/intarray/_int_gin.c
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/gist.h"
#include "access/skey.h"

#include "_int.h"

PG_FUNCTION_INFO_V1(ginint4_queryextract);
Datum		ginint4_queryextract(PG_FUNCTION_ARGS);

Datum
ginint4_queryextract(PG_FUNCTION_ARGS)
{
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *res = NULL;

	*nentries = 0;

	if (strategy == BooleanSearchStrategy)
	{
		QUERYTYPE  *query = PG_GETARG_QUERYTYPE_P(0);
		ITEM	   *items = GETQUERY(query);
		int			i;

		/* empty query must fail */
		if (query->size <= 0)
			PG_RETURN_POINTER(NULL);

		/*
		 * If the query doesn't have any required primitive values (for
		 * instance, it's something like '! 42'), we have to do a full index
		 * scan.
		 */
		if (query_has_required_values(query))
			*searchMode = GIN_SEARCH_MODE_DEFAULT;
		else
			*searchMode = GIN_SEARCH_MODE_ALL;

		/*
		 * Extract all the VAL items as things we want GIN to check for.
		 */
		res = (Datum *) palloc(sizeof(Datum) * query->size);
		*nentries = 0;

		for (i = 0; i < query->size; i++)
		{
			if (items[i].type == VAL)
			{
				res[*nentries] = Int32GetDatum(items[i].val);
				(*nentries)++;
			}
		}
	}
	else
	{
		ArrayType  *query = PG_GETARG_ARRAYTYPE_P(0);

		CHECKARRVALID(query);
		*nentries = ARRNELEMS(query);
		if (*nentries > 0)
		{
			int32	   *arr;
			int32		i;

			res = (Datum *) palloc(sizeof(Datum) * (*nentries));

			arr = ARRPTR(query);
			for (i = 0; i < *nentries; i++)
				res[i] = Int32GetDatum(arr[i]);
		}

		switch (strategy)
		{
			case RTOverlapStrategyNumber:
				*searchMode = GIN_SEARCH_MODE_DEFAULT;
				break;
			case RTContainedByStrategyNumber:
			case RTOldContainedByStrategyNumber:
				/* empty set is contained in everything */
				*searchMode = GIN_SEARCH_MODE_INCLUDE_EMPTY;
				break;
			case RTSameStrategyNumber:
				if (*nentries > 0)
					*searchMode = GIN_SEARCH_MODE_DEFAULT;
				else
					*searchMode = GIN_SEARCH_MODE_INCLUDE_EMPTY;
				break;
			case RTContainsStrategyNumber:
			case RTOldContainsStrategyNumber:
				if (*nentries > 0)
					*searchMode = GIN_SEARCH_MODE_DEFAULT;
				else	/* everything contains the empty set */
					*searchMode = GIN_SEARCH_MODE_ALL;
				break;
			default:
				elog(ERROR, "ginint4_queryextract: unknown strategy number: %d",
					 strategy);
		}
	}

	PG_RETURN_POINTER(res);
}

PG_FUNCTION_INFO_V1(ginint4_consistent);
Datum		ginint4_consistent(PG_FUNCTION_ARGS);

Datum
ginint4_consistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);
	int32		nkeys = PG_GETARG_INT32(3);

	/* Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res = FALSE;
	int32		i;

	switch (strategy)
	{
		case RTOverlapStrategyNumber:
			/* result is not lossy */
			*recheck = false;
			/* at least one element in check[] is true, so result = true */
			res = TRUE;
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
			/* we will need recheck */
			*recheck = true;
			/* at least one element in check[] is true, so result = true */
			res = TRUE;
			break;
		case RTSameStrategyNumber:
			/* we will need recheck */
			*recheck = true;
			/* Must have all elements in check[] true */
			res = TRUE;
			for (i = 0; i < nkeys; i++)
			{
				if (!check[i])
				{
					res = FALSE;
					break;
				}
			}
			break;
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			/* result is not lossy */
			*recheck = false;
			/* Must have all elements in check[] true */
			res = TRUE;
			for (i = 0; i < nkeys; i++)
			{
				if (!check[i])
				{
					res = FALSE;
					break;
				}
			}
			break;
		case BooleanSearchStrategy:
			{
				QUERYTYPE  *query = PG_GETARG_QUERYTYPE_P(2);

				/* result is not lossy */
				*recheck = false;
				res = gin_bool_consistent(query, check);
			}
			break;
		default:
			elog(ERROR, "ginint4_consistent: unknown strategy number: %d",
				 strategy);
	}

	PG_RETURN_BOOL(res);
}
