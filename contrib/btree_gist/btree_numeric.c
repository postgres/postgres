/*
 * contrib/btree_gist/btree_numeric.c
 */
#include "postgres.h"

#include <math.h>
#include <float.h>

#include "btree_gist.h"
#include "btree_utils_var.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/rel.h"

/*
** Bytea ops
*/
PG_FUNCTION_INFO_V1(gbt_numeric_compress);
PG_FUNCTION_INFO_V1(gbt_numeric_union);
PG_FUNCTION_INFO_V1(gbt_numeric_picksplit);
PG_FUNCTION_INFO_V1(gbt_numeric_consistent);
PG_FUNCTION_INFO_V1(gbt_numeric_penalty);
PG_FUNCTION_INFO_V1(gbt_numeric_same);


/* define for comparison */

static bool
gbt_numeric_gt(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(numeric_gt,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_numeric_ge(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(numeric_ge,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_numeric_eq(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(numeric_eq,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_numeric_le(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(numeric_le,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_numeric_lt(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(numeric_lt,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static int32
gbt_numeric_cmp(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_cmp,
											 PointerGetDatum(a),
											 PointerGetDatum(b)));
}


static const gbtree_vinfo tinfo =
{
	gbt_t_numeric,
	0,
	false,
	gbt_numeric_gt,
	gbt_numeric_ge,
	gbt_numeric_eq,
	gbt_numeric_le,
	gbt_numeric_lt,
	gbt_numeric_cmp,
	NULL
};


/**************************************************
 * Text ops
 **************************************************/


Datum
gbt_numeric_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_var_compress(entry, &tinfo));
}



Datum
gbt_numeric_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	void	   *query = (void *) DatumGetNumeric(PG_GETARG_DATUM(1));
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		retval;
	GBT_VARKEY *key = (GBT_VARKEY *) DatumGetPointer(entry->key);
	GBT_VARKEY_R r = gbt_var_key_readable(key);

	/* All cases served by this function are exact */
	*recheck = false;

	retval = gbt_var_consistent(&r, query, strategy, PG_GET_COLLATION(),
								GIST_LEAF(entry), &tinfo, fcinfo->flinfo);
	PG_RETURN_BOOL(retval);
}



Datum
gbt_numeric_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int32	   *size = (int *) PG_GETARG_POINTER(1);

	PG_RETURN_POINTER(gbt_var_union(entryvec, size, PG_GET_COLLATION(),
									&tinfo, fcinfo->flinfo));
}


Datum
gbt_numeric_same(PG_FUNCTION_ARGS)
{
	Datum		d1 = PG_GETARG_DATUM(0);
	Datum		d2 = PG_GETARG_DATUM(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_var_same(d1, d2, PG_GET_COLLATION(), &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}


Datum
gbt_numeric_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *o = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *n = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	Numeric		us,
				os,
				ds;

	GBT_VARKEY *org = (GBT_VARKEY *) DatumGetPointer(o->key);
	GBT_VARKEY *newe = (GBT_VARKEY *) DatumGetPointer(n->key);
	Datum		uni;
	GBT_VARKEY_R rk,
				ok,
				uk;

	rk = gbt_var_key_readable(org);
	uni = PointerGetDatum(gbt_var_key_copy(&rk));
	gbt_var_bin_union(&uni, newe, PG_GET_COLLATION(), &tinfo, fcinfo->flinfo);
	ok = gbt_var_key_readable(org);
	uk = gbt_var_key_readable((GBT_VARKEY *) DatumGetPointer(uni));

	us = DatumGetNumeric(DirectFunctionCall2(numeric_sub,
											 PointerGetDatum(uk.upper),
											 PointerGetDatum(uk.lower)));

	os = DatumGetNumeric(DirectFunctionCall2(numeric_sub,
											 PointerGetDatum(ok.upper),
											 PointerGetDatum(ok.lower)));

	ds = DatumGetNumeric(DirectFunctionCall2(numeric_sub,
											 NumericGetDatum(us),
											 NumericGetDatum(os)));

	if (numeric_is_nan(us))
	{
		if (numeric_is_nan(os))
			*result = 0.0;
		else
			*result = 1.0;
	}
	else
	{
		Numeric		nul = int64_to_numeric(0);

		*result = 0.0;

		if (DirectFunctionCall2(numeric_gt, NumericGetDatum(ds), NumericGetDatum(nul)))
		{
			*result += FLT_MIN;
			os = DatumGetNumeric(DirectFunctionCall2(numeric_div,
													 NumericGetDatum(ds),
													 NumericGetDatum(us)));
			*result += (float4) DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow, NumericGetDatum(os)));
		}
	}

	if (*result > 0)
		*result *= (FLT_MAX / (((GISTENTRY *) PG_GETARG_POINTER(0))->rel->rd_att->natts + 1));

	PG_RETURN_POINTER(result);
}



Datum
gbt_numeric_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

	gbt_var_picksplit(entryvec, v, PG_GET_COLLATION(),
					  &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(v);
}
