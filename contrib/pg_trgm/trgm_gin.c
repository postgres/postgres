/*
 * $PostgreSQL: pgsql/contrib/pg_trgm/trgm_gin.c,v 1.5 2008/07/11 11:56:48 teodor Exp $ 
 */
#include "trgm.h"

#include "access/gin.h"
#include "access/itup.h"
#include "access/tuptoaster.h"
#include "storage/bufpage.h"
#include "utils/array.h"
#include "utils/builtins.h"

PG_FUNCTION_INFO_V1(gin_extract_trgm);
Datum		gin_extract_trgm(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gin_trgm_consistent);
Datum		gin_trgm_consistent(PG_FUNCTION_ARGS);

Datum
gin_extract_trgm(PG_FUNCTION_ARGS)
{
	text	   *val = (text *) PG_GETARG_TEXT_P(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = NULL;
	TRGM	   *trg;
	int4		trglen;

	*nentries = 0;

	trg = generate_trgm(VARDATA(val), VARSIZE(val) - VARHDRSZ);
	trglen = ARRNELEM(trg);

	if (trglen > 0)
	{
		trgm	   *ptr;
		int4		i = 0,
					item;

		*nentries = (int32) trglen;
		entries = (Datum *) palloc(sizeof(Datum) * trglen);

		ptr = GETARR(trg);
		while (ptr - GETARR(trg) < ARRNELEM(trg))
		{
			item = TRGMINT(ptr);
			entries[i++] = Int32GetDatum(item);

			ptr++;
		}
	}

	PG_RETURN_POINTER(entries);
}

/*
 * Per call strage for consistent functions to
 * cache computed value from query
 */
typedef struct PerCallConsistentStorage {
	int		trglen;
	text	data[1]; /* query */
} PerCallConsistentStorage;
#define PCCSHDR_SZ  offsetof(PerCallConsistentStorage, data)

Datum
gin_trgm_consistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	/* StrategyNumber strategy = PG_GETARG_UINT16(1); */
	text	   *query = PG_GETARG_TEXT_P(2);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(3);
	bool		res = FALSE;
	int4		i,
				trglen,
				ntrue = 0;
	PerCallConsistentStorage  *pccs = (PerCallConsistentStorage*) fcinfo->flinfo->fn_extra;

	/* All cases served by this function are inexact */
	*recheck = true;

	if ( pccs == NULL || VARSIZE(pccs->data) != VARSIZE(query) || memcmp( pccs->data, query, VARSIZE(query) ) !=0  )
	{
		TRGM	   *trg = generate_trgm(VARDATA(query), VARSIZE(query) - VARHDRSZ);

		if ( pccs )
			pfree(pccs);

		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, 
									VARSIZE(query) + PCCSHDR_SZ);
		pccs = (PerCallConsistentStorage*) fcinfo->flinfo->fn_extra;

		pccs->trglen = ARRNELEM(trg);
		memcpy( pccs->data, query, VARSIZE(query) );
	}

	trglen = pccs->trglen;

	for (i = 0; i < trglen; i++)
		if (check[i])
			ntrue++;

#ifdef DIVUNION
	res = (trglen == ntrue) ? true : ((((((float4) ntrue) / ((float4) (trglen - ntrue)))) >= trgm_limit) ? true : false);
#else
	res = (trglen == 0) ? false : ((((((float4) ntrue) / ((float4) trglen))) >= trgm_limit) ? true : false);
#endif

	PG_RETURN_BOOL(res);
}
