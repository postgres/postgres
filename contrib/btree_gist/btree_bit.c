/*
 * contrib/btree_gist/btree_bit.c
 *
 * Support for bit and varbit types (which act the same for our purposes).
 *
 * Leaf-page keys are bit/varbit values, but internal-page keys are just
 * bytea values containing the first N bytes of the represented bitstring.
 * (In particular, they lack the bit_len field of a bit/varbit Datum.)
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_var.h"
#include "utils/fmgrprotos.h"
#include "utils/sortsupport.h"
#include "utils/varbit.h"
#include "varatt.h"

/* GiST support functions */
PG_FUNCTION_INFO_V1(gbt_bit_compress);
PG_FUNCTION_INFO_V1(gbt_bit_union);
PG_FUNCTION_INFO_V1(gbt_bit_picksplit);
PG_FUNCTION_INFO_V1(gbt_bit_consistent);
PG_FUNCTION_INFO_V1(gbt_bit_penalty);
PG_FUNCTION_INFO_V1(gbt_bit_same);
PG_FUNCTION_INFO_V1(gbt_bit_sortsupport);
PG_FUNCTION_INFO_V1(gbt_varbit_sortsupport);


/* define for comparison */

static bool
gbt_bitgt(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(bitgt,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_bitge(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(bitge,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_biteq(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(biteq,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_bitle(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(bitle,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_bitlt(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(bitlt,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

/*
 * Notice we use byteacmp here, not bitcmp as you might expect.
 * That's because internal-page keys are bytea.
 */
static int32
gbt_bitcmp(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetInt32(DirectFunctionCall2(byteacmp,
											 PointerGetDatum(a),
											 PointerGetDatum(b)));
}


/*
 * Convert a leaf-page bit/varbit value to internal-page form.
 *
 * The important change here is to remove the bit_len field so that
 * what we have left looks like a bytea.
 *
 * The business with padding to INTALIGN length appears entirely historical,
 * but we can't remove that without breaking on-disk compatibility.
 * For example, if the lower bound of some leaf page is the 20-bit string
 * of all ones, with data contents FFFFF0, this code pads it to bytea FFFFF000
 * in the internal key representing that page.  If, when we search the index
 * for that value, we did not again pad to FFFFF000, then byteacmp would
 * say that the query is strictly less than the lower bound so we would not
 * descend to that leaf page.
 */
static bytea *
gbt_bit_xfrm(const VarBit *leaf)
{
	bytea	   *out;
	int			sz = VARBITBYTES(leaf) + VARHDRSZ;
	int			padded_sz = INTALIGN(sz);

	out = (bytea *) palloc(padded_sz);
	/* initialize the padding bytes to zero */
	while (sz < padded_sz)
		((char *) out)[sz++] = 0;
	SET_VARSIZE(out, padded_sz);
	memcpy(VARDATA(out), VARBITS(leaf), VARBITBYTES(leaf));
	return out;
}

/*
 * Convert a GBT_VARKEY representation of a leaf key to a palloc'd
 * GBT_VARKEY representation of an internal key.
 * We assume lower == upper since it's a leaf key.
 */
static GBT_VARKEY *
gbt_bit_l2n(GBT_VARKEY *leaf, FmgrInfo *flinfo)
{
	GBT_VARKEY *out;
	GBT_VARKEY_R r = gbt_var_key_readable(leaf);
	bytea	   *o;

	o = gbt_bit_xfrm((const VarBit *) r.lower);
	r.upper = r.lower = o;
	out = gbt_var_key_copy(&r);
	pfree(o);

	return out;
}

static const gbtree_vinfo tinfo =
{
	gbt_t_bit,
	true,						/* internal keys can be truncated */
	gbt_bitgt,
	gbt_bitge,
	gbt_biteq,
	gbt_bitle,
	gbt_bitlt,
	gbt_bitcmp,
	gbt_bit_l2n					/* leaf to internal transformation */
};


/**************************************************
 * GiST support functions
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
	VarBit	   *query = PG_GETARG_VARBIT_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
#ifdef NOT_USED
	Oid			subtype = PG_GETARG_OID(3);
#endif
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		retval;
	GBT_VARKEY *key = (GBT_VARKEY *) DatumGetPointer(entry->key);
	GBT_VARKEY_R r = gbt_var_key_readable(key);

	/* All cases served by this function are exact */
	*recheck = false;

	if (GIST_LEAF(entry))
		retval = gbt_var_consistent(&r, query, strategy, PG_GET_COLLATION(),
									true, &tinfo, fcinfo->flinfo);
	else
	{
		/* Must convert to internal form to compare to internal-page entries */
		bytea	   *q = gbt_bit_xfrm(query);

		retval = gbt_var_consistent(&r, q, strategy, PG_GET_COLLATION(),
									false, &tinfo, fcinfo->flinfo);
	}
	PG_RETURN_BOOL(retval);
}

Datum
gbt_bit_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int32	   *size = (int *) PG_GETARG_POINTER(1);

	PG_RETURN_POINTER(gbt_var_union(entryvec, size, PG_GET_COLLATION(),
									&tinfo, fcinfo->flinfo));
}

Datum
gbt_bit_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

	gbt_var_picksplit(entryvec, v, PG_GET_COLLATION(),
					  &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(v);
}

Datum
gbt_bit_same(PG_FUNCTION_ARGS)
{
	Datum		d1 = PG_GETARG_DATUM(0);
	Datum		d2 = PG_GETARG_DATUM(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_var_same(d1, d2, PG_GET_COLLATION(), &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}

Datum
gbt_bit_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *o = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *n = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	PG_RETURN_POINTER(gbt_var_penalty(result, o, n, PG_GET_COLLATION(),
									  &tinfo, fcinfo->flinfo));
}

static int
gbt_bit_ssup_cmp(Datum x, Datum y, SortSupport ssup)
{
	GBT_VARKEY *key1 = PG_DETOAST_DATUM(x);
	GBT_VARKEY *key2 = PG_DETOAST_DATUM(y);

	GBT_VARKEY_R arg1 = gbt_var_key_readable(key1);
	GBT_VARKEY_R arg2 = gbt_var_key_readable(key2);
	Datum		result;

	/* for leaf items we expect lower == upper, so only compare lower */
	result = DirectFunctionCall2(bitcmp,
								 PointerGetDatum(arg1.lower),
								 PointerGetDatum(arg2.lower));

	GBT_FREE_IF_COPY(key1, x);
	GBT_FREE_IF_COPY(key2, y);

	return DatumGetInt32(result);
}

Datum
gbt_bit_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = gbt_bit_ssup_cmp;
	ssup->ssup_extra = NULL;

	PG_RETURN_VOID();
}

Datum
gbt_varbit_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = gbt_bit_ssup_cmp;
	ssup->ssup_extra = NULL;

	PG_RETURN_VOID();
}
