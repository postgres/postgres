/*-------------------------------------------------------------------------
 *
 * dict.c
 *		Standard interface to dictionary
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tsearch/dict.c,v 1.1 2007/08/21 01:11:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/skey.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_type.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_public.h"
#include "tsearch/ts_utils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * Lexize one word by dictionary, mostly debug function
 */
static ArrayType *
ts_lexize_workhorse(Oid dictId, text *in)
{
	TSDictionaryCacheEntry *dict;
	TSLexeme   *res,
			   *ptr;
	Datum	   *da;
	ArrayType  *a;
	DictSubState dstate = {false, false, NULL};

	dict = lookup_ts_dictionary_cache(dictId);

	res = (TSLexeme *) DatumGetPointer(FunctionCall4(&dict->lexize,
											 PointerGetDatum(dict->dictData),
												PointerGetDatum(VARDATA(in)),
									   Int32GetDatum(VARSIZE(in) - VARHDRSZ),
												 PointerGetDatum(&dstate)));

	if (dstate.getnext)
	{
		dstate.isend = true;
		ptr = (TSLexeme *) DatumGetPointer(FunctionCall4(&dict->lexize,
											 PointerGetDatum(dict->dictData),
												PointerGetDatum(VARDATA(in)),
									   Int32GetDatum(VARSIZE(in) - VARHDRSZ),
												 PointerGetDatum(&dstate)));
		if (ptr != NULL)
			res = ptr;
	}

	if (!res)
		return NULL;

	ptr = res;
	while (ptr->lexeme)
		ptr++;
	da = (Datum *) palloc(sizeof(Datum) * (ptr - res + 1));
	ptr = res;
	while (ptr->lexeme)
	{
		da[ptr - res] = DirectFunctionCall1(textin, CStringGetDatum(ptr->lexeme));
		ptr++;
	}

	a = construct_array(da,
						ptr - res,
						TEXTOID,
						-1,
						false,
						'i');

	ptr = res;
	while (ptr->lexeme)
	{
		pfree(DatumGetPointer(da[ptr - res]));
		pfree(ptr->lexeme);
		ptr++;
	}
	pfree(res);
	pfree(da);

	return a;
}

Datum
ts_lexize_byid(PG_FUNCTION_ARGS)
{
	Oid			dictId = PG_GETARG_OID(0);
	text	   *in = PG_GETARG_TEXT_P(1);
	ArrayType  *a;

	a = ts_lexize_workhorse(dictId, in);

	if (a)
		PG_RETURN_POINTER(a);
	else
		PG_RETURN_NULL();
}

Datum
ts_lexize_byname(PG_FUNCTION_ARGS)
{
	text	   *dictname = PG_GETARG_TEXT_P(0);
	text	   *in = PG_GETARG_TEXT_P(1);
	Oid			dictId;
	ArrayType  *a;

	dictId = TSDictionaryGetDictid(textToQualifiedNameList(dictname), false);
	a = ts_lexize_workhorse(dictId, in);

	if (a)
		PG_RETURN_POINTER(a);
	else
		PG_RETURN_NULL();
}
