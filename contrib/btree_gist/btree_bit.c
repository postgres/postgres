#include "btree_gist.h"
#include "btree_utils_var.h"
#include "utils/builtins.h"
#include "utils/varbit.h"


/*
** Bit ops
*/
PG_FUNCTION_INFO_V1(gbt_bit_compress);
PG_FUNCTION_INFO_V1(gbt_bit_union);
PG_FUNCTION_INFO_V1(gbt_bit_picksplit);
PG_FUNCTION_INFO_V1(gbt_bit_consistent);
PG_FUNCTION_INFO_V1(gbt_bit_penalty);
PG_FUNCTION_INFO_V1(gbt_bit_same);

Datum		gbt_bit_compress(PG_FUNCTION_ARGS);
Datum		gbt_bit_union(PG_FUNCTION_ARGS);
Datum		gbt_bit_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_bit_consistent(PG_FUNCTION_ARGS);
Datum		gbt_bit_penalty(PG_FUNCTION_ARGS);
Datum		gbt_bit_same(PG_FUNCTION_ARGS);



/* define for comparison */

static bool
gbt_bitgt(const void *a, const void *b)
{
	return (DatumGetBool(DirectFunctionCall2(bitgt, PointerGetDatum(a), PointerGetDatum(b))));
}

static bool
gbt_bitge(const void *a, const void *b)
{
	return (DatumGetBool(DirectFunctionCall2(bitge, PointerGetDatum(a), PointerGetDatum(b))));
}

static bool
gbt_biteq(const void *a, const void *b)
{
	return (DatumGetBool(DirectFunctionCall2(biteq, PointerGetDatum(a), PointerGetDatum(b))));
}

static bool
gbt_bitle(const void *a, const void *b)
{
	return (DatumGetBool(DirectFunctionCall2(bitle, PointerGetDatum(a), PointerGetDatum(b))));
}

static bool
gbt_bitlt(const void *a, const void *b)
{
	return (DatumGetBool(DirectFunctionCall2(bitlt, PointerGetDatum(a), PointerGetDatum(b))));
}

static int32
gbt_bitcmp(const bytea *a, const bytea *b)
{
	return
		(DatumGetInt32(DirectFunctionCall2(byteacmp, PointerGetDatum(a), PointerGetDatum(b))));
}


static bytea *
gbt_bit_xfrm(bytea *leaf)
{
	bytea	   *out = leaf;
	int			s = INTALIGN(VARBITBYTES(leaf) + VARHDRSZ);

	out = palloc(s);
	SET_VARSIZE(out, s);
	memcpy((void *) VARDATA(out), (void *) VARBITS(leaf), VARBITBYTES(leaf));
	return out;
}




static GBT_VARKEY *
gbt_bit_l2n(GBT_VARKEY * leaf)
{

	GBT_VARKEY *out = leaf;
	GBT_VARKEY_R r = gbt_var_key_readable(leaf);
	bytea	   *o;

	o = gbt_bit_xfrm(r.lower);
	r.upper = r.lower = o;
	out = gbt_var_key_copy(&r, TRUE);
	pfree(o);

	return out;

}

static const gbtree_vinfo tinfo =
{
	gbt_t_bit,
	0,
	TRUE,
	gbt_bitgt,
	gbt_bitge,
	gbt_biteq,
	gbt_bitle,
	gbt_bitlt,
	gbt_bitcmp,
	gbt_bit_l2n
};


/**************************************************
 * Bit ops
 **************************************************/

Datum
gbt_bit_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_var_compress(entry, &tinfo));
}

Datum
gbt_bit_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	void	   *query = (void *) DatumGetByteaP(PG_GETARG_DATUM(1));
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		retval = FALSE;
	GBT_VARKEY *key = (GBT_VARKEY *) DatumGetPointer(entry->key);
	GBT_VARKEY_R r = gbt_var_key_readable(key);

	/* All cases served by this function are exact */
	*recheck = false;

	if (GIST_LEAF(entry))
		retval = gbt_var_consistent(&r, query, &strategy, TRUE, &tinfo);
	else
	{
		bytea	   *q = gbt_bit_xfrm((bytea *) query);

		retval = gbt_var_consistent(&r, (void *) q, &strategy, FALSE, &tinfo);
	}
	PG_RETURN_BOOL(retval);
}



Datum
gbt_bit_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int32	   *size = (int *) PG_GETARG_POINTER(1);

	PG_RETURN_POINTER(gbt_var_union(entryvec, size, &tinfo));
}


Datum
gbt_bit_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

	gbt_var_picksplit(entryvec, v, &tinfo);
	PG_RETURN_POINTER(v);
}

Datum
gbt_bit_same(PG_FUNCTION_ARGS)
{
	Datum		d1 = PG_GETARG_DATUM(0);
	Datum		d2 = PG_GETARG_DATUM(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	PG_RETURN_POINTER(gbt_var_same(result, d1, d2, &tinfo));
}


Datum
gbt_bit_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *o = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *n = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	PG_RETURN_POINTER(gbt_var_penalty(result, o, n, &tinfo));
}
