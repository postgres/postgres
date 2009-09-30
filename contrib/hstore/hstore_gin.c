/*
 * $PostgreSQL: pgsql/contrib/hstore/hstore_gin.c,v 1.7 2009/09/30 19:50:22 tgl Exp $
 */
#include "postgres.h"

#include "access/gin.h"
#include "catalog/pg_type.h"

#include "hstore.h"


#define KEYFLAG		'K'
#define VALFLAG		'V'
#define NULLFLAG	'N'

PG_FUNCTION_INFO_V1(gin_extract_hstore);
Datum		gin_extract_hstore(PG_FUNCTION_ARGS);

static text *
makeitem(char *str, int len)
{
	text	   *item;

	item = (text *) palloc(VARHDRSZ + len + 1);
	SET_VARSIZE(item, VARHDRSZ + len + 1);

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
	HEntry     *hsent = ARRPTR(hs);
	char       *ptr = STRPTR(hs);
	int        count = HS_COUNT(hs);
	int        i;

	*nentries = 2 * count;
	if (count)
		entries = (Datum *) palloc(sizeof(Datum) * 2 * count);

	for (i = 0; i < count; ++i)
	{
		text	   *item;

		item = makeitem(HS_KEY(hsent,ptr,i), HS_KEYLEN(hsent,i));
		*VARDATA(item) = KEYFLAG;
		entries[2*i] = PointerGetDatum(item);

		if (HS_VALISNULL(hsent,i))
		{
			item = makeitem(NULL, 0);
			*VARDATA(item) = NULLFLAG;
		}
		else
		{
			item = makeitem(HS_VAL(hsent,ptr,i), HS_VALLEN(hsent,i));
			*VARDATA(item) = VALFLAG;
		}
		entries[2*i+1] = PointerGetDatum(item);
	}

	PG_RETURN_POINTER(entries);
}

PG_FUNCTION_INFO_V1(gin_extract_hstore_query);
Datum		gin_extract_hstore_query(PG_FUNCTION_ARGS);

Datum
gin_extract_hstore_query(PG_FUNCTION_ARGS)
{
	StrategyNumber strategy = PG_GETARG_UINT16(2);

	if (strategy == HStoreContainsStrategyNumber)
	{
		PG_RETURN_DATUM(DirectFunctionCall2(gin_extract_hstore,
											PG_GETARG_DATUM(0),
											PG_GETARG_DATUM(1)
											));
	}
	else if (strategy == HStoreExistsStrategyNumber)
	{
		text	   *item,
				   *query = PG_GETARG_TEXT_PP(0);
		int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
		Datum	   *entries = NULL;

		*nentries = 1;
		entries = (Datum *) palloc(sizeof(Datum));

		item = makeitem(VARDATA_ANY(query), VARSIZE_ANY_EXHDR(query));
		*VARDATA(item) = KEYFLAG;
		entries[0] = PointerGetDatum(item);

		PG_RETURN_POINTER(entries);
	}
	else if (strategy == HStoreExistsAnyStrategyNumber ||
			 strategy == HStoreExistsAllStrategyNumber)
	{
		ArrayType   *query = PG_GETARG_ARRAYTYPE_P(0);
		Datum      *key_datums;
		bool       *key_nulls;
		int        key_count;
		int        i,j;
		int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
		Datum	   *entries = NULL;
		text       *item;

		deconstruct_array(query,
						  TEXTOID, -1, false, 'i',
						  &key_datums, &key_nulls, &key_count);

		entries = (Datum *) palloc(sizeof(Datum) * key_count);

		for (i = 0, j = 0; i < key_count; ++i)
		{
			if (key_nulls[i])
				continue;
			item = makeitem(VARDATA(key_datums[i]), VARSIZE(key_datums[i]) - VARHDRSZ);
			*VARDATA(item) = KEYFLAG;
			entries[j++] = PointerGetDatum(item);
		}

		*nentries = j ? j : -1;

		PG_RETURN_POINTER(entries);
	}
	else
		elog(ERROR, "Unsupported strategy number: %d", strategy);

	PG_RETURN_POINTER(NULL);
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

	*recheck = false;

	if (strategy == HStoreContainsStrategyNumber)
	{
		int			i;

		/*
		 * Index lost information about correspondence of keys and values, so
		 * we need recheck (pre-8.4 this is handled at SQL level)
		 */
		*recheck = true;
		for (i = 0; res && i < nkeys; i++)
			if (check[i] == false)
				res = false;
	}
	else if (strategy == HStoreExistsStrategyNumber)
	{
		/* Existence of key is guaranteed */
		res = true;
	}
	else if (strategy == HStoreExistsAnyStrategyNumber)
	{
		/* Existence of key is guaranteed */
		res = true;
	}
	else if (strategy == HStoreExistsAllStrategyNumber)
	{
		int        i;

		for (i = 0; res && i < nkeys; ++i)
			if (!check[i])
				res = false;
	}
	else
		elog(ERROR, "Unsupported strategy number: %d", strategy);

	PG_RETURN_BOOL(res);
}
