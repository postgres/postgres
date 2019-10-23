/*
 * contrib/pg_trgm/trgm_gin.c
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/stratnum.h"
#include "fmgr.h"
#include "trgm.h"

PG_FUNCTION_INFO_V1(gin_extract_trgm);
PG_FUNCTION_INFO_V1(gin_extract_value_trgm);
PG_FUNCTION_INFO_V1(gin_extract_query_trgm);
PG_FUNCTION_INFO_V1(gin_trgm_consistent);
PG_FUNCTION_INFO_V1(gin_trgm_triconsistent);

/*
 * This function can only be called if a pre-9.1 version of the GIN operator
 * class definition is present in the catalogs (probably as a consequence
 * of upgrade-in-place).  Cope.
 */
Datum
gin_extract_trgm(PG_FUNCTION_ARGS)
{
	if (PG_NARGS() == 3)
		return gin_extract_value_trgm(fcinfo);
	if (PG_NARGS() == 7)
		return gin_extract_query_trgm(fcinfo);
	elog(ERROR, "unexpected number of arguments to gin_extract_trgm");
	PG_RETURN_NULL();
}

Datum
gin_extract_value_trgm(PG_FUNCTION_ARGS)
{
	text	   *val = (text *) PG_GETARG_TEXT_PP(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = NULL;
	TRGM	   *trg;
	int32		trglen;

	*nentries = 0;

	trg = generate_trgm(VARDATA_ANY(val), VARSIZE_ANY_EXHDR(val));
	trglen = ARRNELEM(trg);

	if (trglen > 0)
	{
		trgm	   *ptr;
		int32		i;

		*nentries = trglen;
		entries = (Datum *) palloc(sizeof(Datum) * trglen);

		ptr = GETARR(trg);
		for (i = 0; i < trglen; i++)
		{
			int32		item = trgm2int(ptr);

			entries[i] = Int32GetDatum(item);
			ptr++;
		}
	}

	PG_RETURN_POINTER(entries);
}

Datum
gin_extract_query_trgm(PG_FUNCTION_ARGS)
{
	text	   *val = (text *) PG_GETARG_TEXT_PP(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);

	/* bool   **pmatch = (bool **) PG_GETARG_POINTER(3); */
	Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);

	/* bool   **nullFlags = (bool **) PG_GETARG_POINTER(5); */
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries = NULL;
	TRGM	   *trg;
	int32		trglen;
	trgm	   *ptr;
	TrgmPackedGraph *graph;
	int32		i;

	switch (strategy)
	{
		case SimilarityStrategyNumber:
		case WordSimilarityStrategyNumber:
		case StrictWordSimilarityStrategyNumber:
			trg = generate_trgm(VARDATA_ANY(val), VARSIZE_ANY_EXHDR(val));
			break;
		case ILikeStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~~* with case-sensitive trigrams");
#endif
			/* FALL THRU */
		case LikeStrategyNumber:

			/*
			 * For wildcard search we extract all the trigrams that every
			 * potentially-matching string must include.
			 */
			trg = generate_wildcard_trgm(VARDATA_ANY(val),
										 VARSIZE_ANY_EXHDR(val));
			break;
		case RegExpICaseStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~* with case-sensitive trigrams");
#endif
			/* FALL THRU */
		case RegExpStrategyNumber:
			trg = createTrgmNFA(val, PG_GET_COLLATION(),
								&graph, CurrentMemoryContext);
			if (trg && ARRNELEM(trg) > 0)
			{
				/*
				 * Successful regex processing: store NFA-like graph as
				 * extra_data.  GIN API requires an array of nentries
				 * Pointers, but we just put the same value in each element.
				 */
				trglen = ARRNELEM(trg);
				*extra_data = (Pointer *) palloc(sizeof(Pointer) * trglen);
				for (i = 0; i < trglen; i++)
					(*extra_data)[i] = (Pointer) graph;
			}
			else
			{
				/* No result: have to do full index scan. */
				*nentries = 0;
				*searchMode = GIN_SEARCH_MODE_ALL;
				PG_RETURN_POINTER(entries);
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			trg = NULL;			/* keep compiler quiet */
			break;
	}

	trglen = ARRNELEM(trg);
	*nentries = trglen;

	if (trglen > 0)
	{
		entries = (Datum *) palloc(sizeof(Datum) * trglen);
		ptr = GETARR(trg);
		for (i = 0; i < trglen; i++)
		{
			int32		item = trgm2int(ptr);

			entries[i] = Int32GetDatum(item);
			ptr++;
		}
	}

	/*
	 * If no trigram was extracted then we have to scan all the index.
	 */
	if (trglen == 0)
		*searchMode = GIN_SEARCH_MODE_ALL;

	PG_RETURN_POINTER(entries);
}

Datum
gin_trgm_consistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* text    *query = PG_GETARG_TEXT_PP(2); */
	int32		nkeys = PG_GETARG_INT32(3);
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res;
	int32		i,
				ntrue;
	double		nlimit;

	/* All cases served by this function are inexact */
	*recheck = true;

	switch (strategy)
	{
		case SimilarityStrategyNumber:
		case WordSimilarityStrategyNumber:
		case StrictWordSimilarityStrategyNumber:
			nlimit = index_strategy_get_limit(strategy);

			/* Count the matches */
			ntrue = 0;
			for (i = 0; i < nkeys; i++)
			{
				if (check[i])
					ntrue++;
			}

			/*--------------------
			 * If DIVUNION is defined then similarity formula is:
			 * c / (len1 + len2 - c)
			 * where c is number of common trigrams and it stands as ntrue in
			 * this code.  Here we don't know value of len2 but we can assume
			 * that c (ntrue) is a lower bound of len2, so upper bound of
			 * similarity is:
			 * c / (len1 + c - c)  => c / len1
			 * If DIVUNION is not defined then similarity formula is:
			 * c / max(len1, len2)
			 * And again, c (ntrue) is a lower bound of len2, but c <= len1
			 * just by definition and, consequently, upper bound of
			 * similarity is just c / len1.
			 * So, independently on DIVUNION the upper bound formula is the same.
			 */
			res = (nkeys == 0) ? false :
				(((((float4) ntrue) / ((float4) nkeys))) >= nlimit);
			break;
		case ILikeStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~~* with case-sensitive trigrams");
#endif
			/* FALL THRU */
		case LikeStrategyNumber:
			/* Check if all extracted trigrams are presented. */
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
		case RegExpICaseStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~* with case-sensitive trigrams");
#endif
			/* FALL THRU */
		case RegExpStrategyNumber:
			if (nkeys < 1)
			{
				/* Regex processing gave no result: do full index scan */
				res = true;
			}
			else
				res = trigramsMatchGraph((TrgmPackedGraph *) extra_data[0],
										 check);
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			res = false;		/* keep compiler quiet */
			break;
	}

	PG_RETURN_BOOL(res);
}

/*
 * In all cases, GIN_TRUE is at least as favorable to inclusion as
 * GIN_MAYBE. If no better option is available, simply treat
 * GIN_MAYBE as if it were GIN_TRUE and apply the same test as the binary
 * consistent function.
 */
Datum
gin_trgm_triconsistent(PG_FUNCTION_ARGS)
{
	GinTernaryValue *check = (GinTernaryValue *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* text    *query = PG_GETARG_TEXT_PP(2); */
	int32		nkeys = PG_GETARG_INT32(3);
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	GinTernaryValue res = GIN_MAYBE;
	int32		i,
				ntrue;
	bool	   *boolcheck;
	double		nlimit;

	switch (strategy)
	{
		case SimilarityStrategyNumber:
		case WordSimilarityStrategyNumber:
		case StrictWordSimilarityStrategyNumber:
			nlimit = index_strategy_get_limit(strategy);

			/* Count the matches */
			ntrue = 0;
			for (i = 0; i < nkeys; i++)
			{
				if (check[i] != GIN_FALSE)
					ntrue++;
			}

			/*
			 * See comment in gin_trgm_consistent() about * upper bound
			 * formula
			 */
			res = (nkeys == 0)
				? GIN_FALSE : (((((float4) ntrue) / ((float4) nkeys)) >= nlimit)
							   ? GIN_MAYBE : GIN_FALSE);
			break;
		case ILikeStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~~* with case-sensitive trigrams");
#endif
			/* FALL THRU */
		case LikeStrategyNumber:
			/* Check if all extracted trigrams are presented. */
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
		case RegExpICaseStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~* with case-sensitive trigrams");
#endif
			/* FALL THRU */
		case RegExpStrategyNumber:
			if (nkeys < 1)
			{
				/* Regex processing gave no result: do full index scan */
				res = GIN_MAYBE;
			}
			else
			{
				/*
				 * As trigramsMatchGraph implements a monotonic boolean
				 * function, promoting all GIN_MAYBE keys to GIN_TRUE will
				 * give a conservative result.
				 */
				boolcheck = (bool *) palloc(sizeof(bool) * nkeys);
				for (i = 0; i < nkeys; i++)
					boolcheck[i] = (check[i] != GIN_FALSE);
				if (!trigramsMatchGraph((TrgmPackedGraph *) extra_data[0],
										boolcheck))
					res = GIN_FALSE;
				pfree(boolcheck);
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			res = GIN_FALSE;	/* keep compiler quiet */
			break;
	}

	/* All cases served by this function are inexact */
	Assert(res != GIN_TRUE);
	PG_RETURN_GIN_TERNARY_VALUE(res);
}
