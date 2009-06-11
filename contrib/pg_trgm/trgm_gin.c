/*
 * $PostgreSQL: pgsql/contrib/pg_trgm/trgm_gin.c,v 1.8 2009/06/11 14:48:51 momjian Exp $
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
			item = trgm2int(ptr);
			entries[i++] = Int32GetDatum(item);

			ptr++;
		}
		if (PG_NARGS() > 4)
		{
			/*
			 * Function called from query extracting
			 */
			Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);

			*extra_data = (Pointer *) palloc0(sizeof(Pointer) * (*nentries));

			*(int32 *) (*extra_data) = trglen;
		}
	}

	PG_RETURN_POINTER(entries);
}

Datum
gin_trgm_consistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);

	/* StrategyNumber strategy = PG_GETARG_UINT16(1); */
	/* text    *query = PG_GETARG_TEXT_P(2); */
	/* int32	nkeys = PG_GETARG_INT32(3); */
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res = FALSE;
	int4		i,
				trglen,
				ntrue = 0;

	/* All cases served by this function are inexact */
	*recheck = true;

	trglen = *(int32 *) extra_data;

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
