#include "postgres.h"

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
	Datum	   *res = NULL;

	*nentries = 0;

	if (strategy == BooleanSearchStrategy)
	{
		QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM_COPY(PG_GETARG_POINTER(0));
		ITEM	   *items = GETQUERY(query);
		int			i;

		if (query->size == 0)
			PG_RETURN_POINTER(NULL);

		if (shorterquery(items, query->size) == 0)
			elog(ERROR, "Query requires full scan, GIN doesn't support it");

		pfree(query);

		query = (QUERYTYPE *) PG_DETOAST_DATUM(PG_GETARG_POINTER(0));
		items = GETQUERY(query);

		res = (Datum *) palloc(sizeof(Datum) * query->size);
		*nentries = 0;

		for (i = 0; i < query->size; i++)
			if (items[i].type == VAL)
			{
				res[*nentries] = Int32GetDatum(items[i].val);
				(*nentries)++;
			}
	}
	else
	{
		ArrayType  *query = PG_GETARG_ARRAYTYPE_P(0);
		int4	   *arr;
		uint32		i;

		CHECKARRVALID(query);
		*nentries = ARRNELEMS(query);
		if (*nentries > 0)
		{
			res = (Datum *) palloc(sizeof(Datum) * (*nentries));

			arr = ARRPTR(query);
			for (i = 0; i < *nentries; i++)
				res[i] = Int32GetDatum(arr[i]);
		}
	}

	if (nentries == 0)
	{
		switch (strategy)
		{
			case BooleanSearchStrategy:
			case RTOverlapStrategyNumber:
				*nentries = -1; /* nobody can be found */
				break;
			default:			/* require fullscan: GIN can't find void
								 * arrays */
				break;
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
	bool	   *recheck = (bool *) PG_GETARG_POINTER(3);
	bool		res = FALSE;

	/*
	 * we need not check array carefully, it's done by previous
	 * ginarrayextract call
	 */

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
			{
				ArrayType  *query = PG_GETARG_ARRAYTYPE_P(2);
				int			i,
							nentries = ARRNELEMS(query);

				/* we will need recheck */
				*recheck = true;
				res = TRUE;
				for (i = 0; i < nentries; i++)
					if (!check[i])
					{
						res = FALSE;
						break;
					}
			}
			break;
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			{
				ArrayType  *query = PG_GETARG_ARRAYTYPE_P(2);
				int			i,
							nentries = ARRNELEMS(query);

				/* result is not lossy */
				*recheck = false;
				res = TRUE;
				for (i = 0; i < nentries; i++)
					if (!check[i])
					{
						res = FALSE;
						break;
					}
			}
			break;
		case BooleanSearchStrategy:
			{
				QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM(PG_GETARG_POINTER(2));

				/* result is not lossy */
				*recheck = false;
				res = ginconsistent(query, check);
			}
			break;
		default:
			elog(ERROR, "ginint4_consistent: unknown strategy number: %d",
				 strategy);
	}

	PG_RETURN_BOOL(res);
}
