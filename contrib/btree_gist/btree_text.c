/*
 * contrib/btree_gist/btree_text.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_var.h"
#include "mb/pg_wchar.h"
#include "utils/fmgrprotos.h"

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


/* define for comparison */

static bool
gbt_textgt(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2Coll(text_gt,
												collation,
												PointerGetDatum(a),
												PointerGetDatum(b)));
}

static bool
gbt_textge(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2Coll(text_ge,
												collation,
												PointerGetDatum(a),
												PointerGetDatum(b)));
}

static bool
gbt_texteq(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2Coll(texteq,
												collation,
												PointerGetDatum(a),
												PointerGetDatum(b)));
}

static bool
gbt_textle(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2Coll(text_le,
												collation,
												PointerGetDatum(a),
												PointerGetDatum(b)));
}

static bool
gbt_textlt(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2Coll(text_lt,
												collation,
												PointerGetDatum(a),
												PointerGetDatum(b)));
}

static int32
gbt_textcmp(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetInt32(DirectFunctionCall2Coll(bttextcmp,
												 collation,
												 PointerGetDatum(a),
												 PointerGetDatum(b)));
}

static gbtree_vinfo tinfo =
{
	gbt_t_text,
	0,
	false,
	gbt_textgt,
	gbt_textge,
	gbt_texteq,
	gbt_textle,
	gbt_textlt,
	gbt_textcmp,
	NULL
};

/* bpchar needs its own comparison rules */

static bool
gbt_bpchargt(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2Coll(bpchargt,
												collation,
												PointerGetDatum(a),
												PointerGetDatum(b)));
}

static bool
gbt_bpcharge(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2Coll(bpcharge,
												collation,
												PointerGetDatum(a),
												PointerGetDatum(b)));
}

static bool
gbt_bpchareq(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2Coll(bpchareq,
												collation,
												PointerGetDatum(a),
												PointerGetDatum(b)));
}

static bool
gbt_bpcharle(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2Coll(bpcharle,
												collation,
												PointerGetDatum(a),
												PointerGetDatum(b)));
}

static bool
gbt_bpcharlt(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2Coll(bpcharlt,
												collation,
												PointerGetDatum(a),
												PointerGetDatum(b)));
}

static int32
gbt_bpcharcmp(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetInt32(DirectFunctionCall2Coll(bpcharcmp,
												 collation,
												 PointerGetDatum(a),
												 PointerGetDatum(b)));
}

static gbtree_vinfo bptinfo =
{
	gbt_t_bpchar,
	0,
	false,
	gbt_bpchargt,
	gbt_bpcharge,
	gbt_bpchareq,
	gbt_bpcharle,
	gbt_bpcharlt,
	gbt_bpcharcmp,
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
	/* This should never have been distinct from gbt_text_compress */
	return gbt_text_compress(fcinfo);
}



Datum
gbt_text_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	void	   *query = DatumGetTextP(PG_GETARG_DATUM(1));
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

	retval = gbt_var_consistent(&r, query, strategy, PG_GET_COLLATION(),
								GIST_LEAF(entry), &tinfo, fcinfo->flinfo);

	PG_RETURN_BOOL(retval);
}


Datum
gbt_bpchar_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	void	   *query = DatumGetTextP(PG_GETARG_DATUM(1));
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		retval;
	GBT_VARKEY *key = (GBT_VARKEY *) DatumGetPointer(entry->key);
	GBT_VARKEY_R r = gbt_var_key_readable(key);

	/* All cases served by this function are exact */
	*recheck = false;

	if (bptinfo.eml == 0)
	{
		bptinfo.eml = pg_database_encoding_max_length();
	}

	retval = gbt_var_consistent(&r, query, strategy, PG_GET_COLLATION(),
								GIST_LEAF(entry), &bptinfo, fcinfo->flinfo);
	PG_RETURN_BOOL(retval);
}


Datum
gbt_text_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int32	   *size = (int *) PG_GETARG_POINTER(1);

	PG_RETURN_POINTER(gbt_var_union(entryvec, size, PG_GET_COLLATION(),
									&tinfo, fcinfo->flinfo));
}


Datum
gbt_text_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

	gbt_var_picksplit(entryvec, v, PG_GET_COLLATION(),
					  &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(v);
}

Datum
gbt_text_same(PG_FUNCTION_ARGS)
{
	Datum		d1 = PG_GETARG_DATUM(0);
	Datum		d2 = PG_GETARG_DATUM(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_var_same(d1, d2, PG_GET_COLLATION(), &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}


Datum
gbt_text_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *o = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *n = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	PG_RETURN_POINTER(gbt_var_penalty(result, o, n, PG_GET_COLLATION(),
									  &tinfo, fcinfo->flinfo));
}
