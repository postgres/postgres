#include "btree_gist.h"
#include "btree_utils_var.h"
#include "utils/builtins.h"

/*
** Text ops
*/
PG_FUNCTION_INFO_V1(gbt_text_compress);
PG_FUNCTION_INFO_V1(gbt_bpchar_compress);
PG_FUNCTION_INFO_V1(gbt_text_union);
PG_FUNCTION_INFO_V1(gbt_text_picksplit);
PG_FUNCTION_INFO_V1(gbt_text_consistent);
PG_FUNCTION_INFO_V1(gbt_bpchar_consistent);
PG_FUNCTION_INFO_V1(gbt_text_penalty);
PG_FUNCTION_INFO_V1(gbt_text_same);

Datum		gbt_text_compress(PG_FUNCTION_ARGS);
Datum		gbt_bpchar_compress(PG_FUNCTION_ARGS);
Datum		gbt_text_union(PG_FUNCTION_ARGS);
Datum		gbt_text_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_text_consistent(PG_FUNCTION_ARGS);
Datum		gbt_bpchar_consistent(PG_FUNCTION_ARGS);
Datum		gbt_text_penalty(PG_FUNCTION_ARGS);
Datum		gbt_text_same(PG_FUNCTION_ARGS);


/* define for comparison */

static bool
gbt_textgt(const void *a, const void *b)
{
	return (DatumGetBool(DirectFunctionCall2(text_gt, PointerGetDatum(a), PointerGetDatum(b))));
}

static bool
gbt_textge(const void *a, const void *b)
{
	return (DatumGetBool(DirectFunctionCall2(text_ge, PointerGetDatum(a), PointerGetDatum(b))));
}

static bool
gbt_texteq(const void *a, const void *b)
{
	return (DatumGetBool(DirectFunctionCall2(texteq, PointerGetDatum(a), PointerGetDatum(b))));
}

static bool
gbt_textle(const void *a, const void *b)
{
	return (DatumGetBool(DirectFunctionCall2(text_le, PointerGetDatum(a), PointerGetDatum(b))));
}

static bool
gbt_textlt(const void *a, const void *b)
{
	return (DatumGetBool(DirectFunctionCall2(text_lt, PointerGetDatum(a), PointerGetDatum(b))));
}

static int32
gbt_textcmp(const bytea *a, const bytea *b)
{
	return DatumGetInt32(DirectFunctionCall2(bttextcmp, PointerGetDatum(a), PointerGetDatum(b)));
}

static gbtree_vinfo tinfo =
{
	gbt_t_text,
	0,
	FALSE,
	gbt_textgt,
	gbt_textge,
	gbt_texteq,
	gbt_textle,
	gbt_textlt,
	gbt_textcmp,
	NULL
};


/**************************************************
 * Text ops
 **************************************************/


Datum
gbt_text_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	if (tinfo.eml == 0)
	{
		tinfo.eml = pg_database_encoding_max_length();
	}

	PG_RETURN_POINTER(gbt_var_compress(entry, &tinfo));
}

Datum
gbt_bpchar_compress(PG_FUNCTION_ARGS)
{

	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (tinfo.eml == 0)
	{
		tinfo.eml = pg_database_encoding_max_length();
	}

	if (entry->leafkey)
	{

		Datum		d = DirectFunctionCall1(rtrim1, entry->key);
		GISTENTRY	trim;

		gistentryinit(trim, d,
					  entry->rel, entry->page,
					  entry->offset, TRUE);
		retval = gbt_var_compress(&trim, &tinfo);
	}
	else
		retval = entry;

	PG_RETURN_POINTER(retval);
}



Datum
gbt_text_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	void	   *query = (void *) DatumGetTextP(PG_GETARG_DATUM(1));
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		retval;
	GBT_VARKEY *key = (GBT_VARKEY *) DatumGetPointer(entry->key);
	GBT_VARKEY_R r = gbt_var_key_readable(key);

	/* All cases served by this function are exact */
	*recheck = false;

	if (tinfo.eml == 0)
	{
		tinfo.eml = pg_database_encoding_max_length();
	}

	retval = gbt_var_consistent(&r, query, &strategy, GIST_LEAF(entry), &tinfo);

	PG_RETURN_BOOL(retval);
}


Datum
gbt_bpchar_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	void	   *query = (void *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(1)));
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		retval;
	GBT_VARKEY *key = (GBT_VARKEY *) DatumGetPointer(entry->key);
	GBT_VARKEY_R r = gbt_var_key_readable(key);
	void	   *trim = (void *) DatumGetPointer(DirectFunctionCall1(rtrim1, PointerGetDatum(query)));

	/* All cases served by this function are exact */
	*recheck = false;

	if (tinfo.eml == 0)
	{
		tinfo.eml = pg_database_encoding_max_length();
	}

	retval = gbt_var_consistent(&r, trim, &strategy, GIST_LEAF(entry), &tinfo);
	PG_RETURN_BOOL(retval);
}


Datum
gbt_text_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int32	   *size = (int *) PG_GETARG_POINTER(1);

	PG_RETURN_POINTER(gbt_var_union(entryvec, size, &tinfo));
}


Datum
gbt_text_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

	gbt_var_picksplit(entryvec, v, &tinfo);
	PG_RETURN_POINTER(v);
}

Datum
gbt_text_same(PG_FUNCTION_ARGS)
{
	Datum		d1 = PG_GETARG_DATUM(0);
	Datum		d2 = PG_GETARG_DATUM(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	PG_RETURN_POINTER(gbt_var_same(result, d1, d2, &tinfo));
}


Datum
gbt_text_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *o = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *n = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	PG_RETURN_POINTER(gbt_var_penalty(result, o, n, &tinfo));
}
