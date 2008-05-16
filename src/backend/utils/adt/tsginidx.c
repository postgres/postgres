/*-------------------------------------------------------------------------
 *
 * tsginidx.c
 *	 GIN support functions for tsvector_ops
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/tsginidx.c,v 1.12 2008/05/16 16:31:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/skey.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"


Datum
gin_cmp_tslexeme(PG_FUNCTION_ARGS)
{
	text    *a = PG_GETARG_TEXT_P(0);
	text    *b = PG_GETARG_TEXT_P(1);
	int     cmp;

	cmp = tsCompareString(
					VARDATA(a), VARSIZE(a) - VARHDRSZ,
					VARDATA(b), VARSIZE(b) - VARHDRSZ,
					false );

	PG_FREE_IF_COPY(a,0);
	PG_FREE_IF_COPY(b,1);
	PG_RETURN_INT32( cmp );
}

Datum
gin_cmp_prefix(PG_FUNCTION_ARGS)
{
	text    *a = PG_GETARG_TEXT_P(0);
	text    *b = PG_GETARG_TEXT_P(1);
#ifdef NOT_USED
	StrategyNumber strategy = PG_GETARG_UINT16(2);
#endif
	int     cmp;

	cmp = tsCompareString(
					VARDATA(a), VARSIZE(a) - VARHDRSZ,
					VARDATA(b), VARSIZE(b) - VARHDRSZ,
					true );

	if ( cmp < 0 )
		cmp = 1;  /* prevent continue scan */

	PG_FREE_IF_COPY(a,0);
	PG_FREE_IF_COPY(b,1);
	PG_RETURN_INT32( cmp );
}

Datum
gin_extract_tsvector(PG_FUNCTION_ARGS)
{
	TSVector	vector = PG_GETARG_TSVECTOR(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = NULL;

	*nentries = vector->size;
	if (vector->size > 0)
	{
		int			i;
		WordEntry  *we = ARRPTR(vector);

		entries = (Datum *) palloc(sizeof(Datum) * vector->size);

		for (i = 0; i < vector->size; i++)
		{
			text	   *txt;

			txt = cstring_to_text_with_len(STRPTR(vector) + we->pos, we->len);
			entries[i] = PointerGetDatum(txt);

			we++;
		}
	}

	PG_FREE_IF_COPY(vector, 0);
	PG_RETURN_POINTER(entries);
}

Datum
gin_extract_tsquery(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	/* StrategyNumber strategy = PG_GETARG_UINT16(2); */
	bool      **ptr_partialmatch = (bool**) PG_GETARG_POINTER(3);
	Datum	   *entries = NULL;
	bool       *partialmatch;

	*nentries = 0;

	if (query->size > 0)
	{
		int4		i,
					j = 0,
					len;
		QueryItem  *item;
		bool		use_fullscan=false;

		item = clean_NOT(GETQUERY(query), &len);
		if (!item)
		{
			use_fullscan = true;
			*nentries = 1;
		}

		item = GETQUERY(query);

		for (i = 0; i < query->size; i++)
			if (item[i].type == QI_VAL)
				(*nentries)++;

		entries = (Datum *) palloc(sizeof(Datum) * (*nentries));
		partialmatch = *ptr_partialmatch = (bool*) palloc(sizeof(bool) * (*nentries));

		for (i = 0; i < query->size; i++)
			if (item[i].type == QI_VAL)
			{
				text	   *txt;
				QueryOperand *val = &item[i].operand;

				txt = cstring_to_text_with_len(GETOPERAND(query) + val->distance,
											   val->length);
				partialmatch[j] = val->prefix;
				entries[j++] = PointerGetDatum(txt);
			}

		if ( use_fullscan )
			entries[j++] = PointerGetDatum(cstring_to_text_with_len("", 0));
	}
	else
		*nentries = -1;			/* nothing can be found */

	PG_FREE_IF_COPY(query, 0);

	PG_RETURN_POINTER(entries);
}

typedef struct
{
	QueryItem  *frst;
	bool	   *mapped_check;
	bool	   *need_recheck;
} GinChkVal;

static bool
checkcondition_gin(void *checkval, QueryOperand *val)
{
	GinChkVal  *gcv = (GinChkVal *) checkval;

	/* if any val requiring a weight is used, set recheck flag */
	if (val->weight != 0)
		*(gcv->need_recheck) = true;

	return gcv->mapped_check[((QueryItem *) val) - gcv->frst];
}

Datum
gin_tsquery_consistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	/* StrategyNumber strategy = PG_GETARG_UINT16(1); */
	TSQuery		query = PG_GETARG_TSQUERY(2);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(3);
	bool		res = FALSE;

	/* The query requires recheck only if it involves weights */
	*recheck = false;

	if (query->size > 0)
	{
		int			i,
					j = 0;
		QueryItem  *item;
		GinChkVal	gcv;

		/*
		 * check-parameter array has one entry for each value (operand) in the
		 * query. We expand that array into mapped_check, so that there's one
		 * entry in mapped_check for every node in the query, including
		 * operators, to allow quick lookups in checkcondition_gin. Only the
		 * entries corresponding operands are actually used.
		 */

		gcv.frst = item = GETQUERY(query);
		gcv.mapped_check = (bool *) palloc(sizeof(bool) * query->size);
		gcv.need_recheck = recheck;

		for (i = 0; i < query->size; i++)
			if (item[i].type == QI_VAL)
				gcv.mapped_check[i] = check[j++];

		res = TS_execute(
						 GETQUERY(query),
						 &gcv,
						 true,
						 checkcondition_gin
			);
	}

	PG_RETURN_BOOL(res);
}
