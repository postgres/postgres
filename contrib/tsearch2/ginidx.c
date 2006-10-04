#include "postgres.h"

#include <float.h>

#include "access/gist.h"
#include "access/itup.h"
#include "access/skey.h"
#include "access/tuptoaster.h"
#include "storage/bufpage.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "tsvector.h"
#include "query.h"
#include "query_cleanup.h"

PG_FUNCTION_INFO_V1(gin_extract_tsvector);
Datum		gin_extract_tsvector(PG_FUNCTION_ARGS);

Datum
gin_extract_tsvector(PG_FUNCTION_ARGS)
{
	tsvector   *vector = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	uint32	   *nentries = (uint32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = NULL;

	*nentries = 0;
	if (vector->size > 0)
	{
		int			i;
		WordEntry  *we = ARRPTR(vector);

		*nentries = (uint32) vector->size;
		entries = (Datum *) palloc(sizeof(Datum) * vector->size);

		for (i = 0; i < vector->size; i++)
		{
			text	   *txt = (text *) palloc(VARHDRSZ + we->len);

			VARATT_SIZEP(txt) = VARHDRSZ + we->len;
			memcpy(VARDATA(txt), STRPTR(vector) + we->pos, we->len);

			entries[i] = PointerGetDatum(txt);

			we++;
		}
	}

	PG_FREE_IF_COPY(vector, 0);
	PG_RETURN_POINTER(entries);
}


PG_FUNCTION_INFO_V1(gin_extract_tsquery);
Datum		gin_extract_tsquery(PG_FUNCTION_ARGS);

Datum
gin_extract_tsquery(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	uint32	   *nentries = (uint32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = DatumGetUInt16(PG_GETARG_DATUM(2));
	Datum	   *entries = NULL;

	*nentries = 0;
	if (query->size > 0)
	{
		int4		i,
					j = 0,
					len;
		ITEM	   *item;

		item = clean_NOT_v2(GETQUERY(query), &len);
		if (!item)
			elog(ERROR, "Query requires full scan, GIN doesn't support it");

		item = GETQUERY(query);

		for (i = 0; i < query->size; i++)
			if (item[i].type == VAL)
				(*nentries)++;

		entries = (Datum *) palloc(sizeof(Datum) * (*nentries));

		for (i = 0; i < query->size; i++)
			if (item[i].type == VAL)
			{
				text	   *txt;

				txt = (text *) palloc(VARHDRSZ + item[i].length);

				VARATT_SIZEP(txt) = VARHDRSZ + item[i].length;
				memcpy(VARDATA(txt), GETOPERAND(query) + item[i].distance, item[i].length);

				entries[j++] = PointerGetDatum(txt);

				if (strategy == 1 && item[i].weight != 0)
					elog(ERROR, "With class of lexeme restrictions use @@@ operation");
			}

	}

	PG_FREE_IF_COPY(query, 0);
	PG_RETURN_POINTER(entries);
}

typedef struct
{
	ITEM	   *frst;
	bool	   *mapped_check;
}	GinChkVal;

static bool
checkcondition_gin(void *checkval, ITEM * val)
{
	GinChkVal  *gcv = (GinChkVal *) checkval;

	return gcv->mapped_check[val - gcv->frst];
}

PG_FUNCTION_INFO_V1(gin_ts_consistent);
Datum		gin_ts_consistent(PG_FUNCTION_ARGS);

Datum
gin_ts_consistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM(PG_GETARG_DATUM(2));
	bool		res = FALSE;

	if (query->size > 0)
	{
		int4		i,
					j = 0;
		ITEM	   *item;
		GinChkVal	gcv;

		gcv.frst = item = GETQUERY(query);
		gcv.mapped_check = (bool *) palloc(sizeof(bool) * query->size);

		for (i = 0; i < query->size; i++)
			if (item[i].type == VAL)
				gcv.mapped_check[i] = check[j++];


		res = TS_execute(
						 GETQUERY(query),
						 &gcv,
						 true,
						 checkcondition_gin
			);

	}

	PG_FREE_IF_COPY(query, 2);
	PG_RETURN_BOOL(res);
}
