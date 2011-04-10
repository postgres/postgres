/*
 * contrib/hstore/hstore_gin.c
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/skey.h"
#include "catalog/pg_type.h"

#include "hstore.h"


/*
 * When using a GIN index for hstore, we choose to index both keys and values.
 * The storage format is "text" values, with K, V, or N prepended to the string
 * to indicate key, value, or null values.	(As of 9.1 it might be better to
 * store null values as nulls, but we'll keep it this way for on-disk
 * compatibility.)
 */
#define KEYFLAG		'K'
#define VALFLAG		'V'
#define NULLFLAG	'N'

PG_FUNCTION_INFO_V1(gin_extract_hstore);
Datum		gin_extract_hstore(PG_FUNCTION_ARGS);

/* Build an indexable text value */
static text *
makeitem(char *str, int len, char flag)
{
	text	   *item;

	item = (text *) palloc(VARHDRSZ + len + 1);
	SET_VARSIZE(item, VARHDRSZ + len + 1);

	*VARDATA(item) = flag;

	if (str && len > 0)
		memcpy(VARDATA(item) + 1, str, len);

	return item;
}

Datum
gin_extract_hstore(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HS(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = NULL;
	HEntry	   *hsent = ARRPTR(hs);
	char	   *ptr = STRPTR(hs);
	int			count = HS_COUNT(hs);
	int			i;

	*nentries = 2 * count;
	if (count)
		entries = (Datum *) palloc(sizeof(Datum) * 2 * count);

	for (i = 0; i < count; ++i)
	{
		text	   *item;

		item = makeitem(HS_KEY(hsent, ptr, i), HS_KEYLEN(hsent, i),
						KEYFLAG);
		entries[2 * i] = PointerGetDatum(item);

		if (HS_VALISNULL(hsent, i))
			item = makeitem(NULL, 0, NULLFLAG);
		else
			item = makeitem(HS_VAL(hsent, ptr, i), HS_VALLEN(hsent, i),
							VALFLAG);
		entries[2 * i + 1] = PointerGetDatum(item);
	}

	PG_RETURN_POINTER(entries);
}

PG_FUNCTION_INFO_V1(gin_extract_hstore_query);
Datum		gin_extract_hstore_query(PG_FUNCTION_ARGS);

Datum
gin_extract_hstore_query(PG_FUNCTION_ARGS)
{
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries;

	if (strategy == HStoreContainsStrategyNumber)
	{
		/* Query is an hstore, so just apply gin_extract_hstore... */
		entries = (Datum *)
			DatumGetPointer(DirectFunctionCall2(gin_extract_hstore,
												PG_GETARG_DATUM(0),
												PointerGetDatum(nentries)));
		/* ... except that "contains {}" requires a full index scan */
		if (entries == NULL)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else if (strategy == HStoreExistsStrategyNumber)
	{
		text	   *query = PG_GETARG_TEXT_PP(0);
		text	   *item;

		*nentries = 1;
		entries = (Datum *) palloc(sizeof(Datum));
		item = makeitem(VARDATA_ANY(query), VARSIZE_ANY_EXHDR(query), KEYFLAG);
		entries[0] = PointerGetDatum(item);
	}
	else if (strategy == HStoreExistsAnyStrategyNumber ||
			 strategy == HStoreExistsAllStrategyNumber)
	{
		ArrayType  *query = PG_GETARG_ARRAYTYPE_P(0);
		Datum	   *key_datums;
		bool	   *key_nulls;
		int			key_count;
		int			i,
					j;
		text	   *item;

		deconstruct_array(query,
						  TEXTOID, -1, false, 'i',
						  &key_datums, &key_nulls, &key_count);

		entries = (Datum *) palloc(sizeof(Datum) * key_count);

		for (i = 0, j = 0; i < key_count; ++i)
		{
			/* Nulls in the array are ignored, cf hstoreArrayToPairs */
			if (key_nulls[i])
				continue;
			item = makeitem(VARDATA(key_datums[i]), VARSIZE(key_datums[i]) - VARHDRSZ, KEYFLAG);
			entries[j++] = PointerGetDatum(item);
		}

		*nentries = j;
		/* ExistsAll with no keys should match everything */
		if (j == 0 && strategy == HStoreExistsAllStrategyNumber)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else
	{
		elog(ERROR, "unrecognized strategy number: %d", strategy);
		entries = NULL;			/* keep compiler quiet */
	}

	PG_RETURN_POINTER(entries);
}

PG_FUNCTION_INFO_V1(gin_consistent_hstore);
Datum		gin_consistent_hstore(PG_FUNCTION_ARGS);

Datum
gin_consistent_hstore(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* HStore	   *query = PG_GETARG_HS(2); */
	int32		nkeys = PG_GETARG_INT32(3);

	/* Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res = true;
	int32		i;

	if (strategy == HStoreContainsStrategyNumber)
	{
		/*
		 * Index doesn't have information about correspondence of keys and
		 * values, so we need recheck.	However, if not all the keys are
		 * present, we can fail at once.
		 */
		*recheck = true;
		for (i = 0; i < nkeys; i++)
		{
			if (!check[i])
			{
				res = false;
				break;
			}
		}
	}
	else if (strategy == HStoreExistsStrategyNumber)
	{
		/* Existence of key is guaranteed in default search mode */
		*recheck = false;
		res = true;
	}
	else if (strategy == HStoreExistsAnyStrategyNumber)
	{
		/* Existence of key is guaranteed in default search mode */
		*recheck = false;
		res = true;
	}
	else if (strategy == HStoreExistsAllStrategyNumber)
	{
		/* Testing for all the keys being present gives an exact result */
		*recheck = false;
		for (i = 0; i < nkeys; i++)
		{
			if (!check[i])
			{
				res = false;
				break;
			}
		}
	}
	else
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	PG_RETURN_BOOL(res);
}
