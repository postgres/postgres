/*
 * contrib/btree_gist/btree_float4.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/float.h"

typedef struct float4key
{
	float4		lower;
	float4		upper;
} float4KEY;

/*
** float4 ops
*/
PG_FUNCTION_INFO_V1(gbt_float4_compress);
PG_FUNCTION_INFO_V1(gbt_float4_fetch);
PG_FUNCTION_INFO_V1(gbt_float4_union);
PG_FUNCTION_INFO_V1(gbt_float4_picksplit);
PG_FUNCTION_INFO_V1(gbt_float4_consistent);
PG_FUNCTION_INFO_V1(gbt_float4_distance);
PG_FUNCTION_INFO_V1(gbt_float4_penalty);
PG_FUNCTION_INFO_V1(gbt_float4_same);

/*
 * Use the NaN-aware comparators from utils/float.h, so that our results
 * will agree with standard btree indexes.  Note that penalty and distance
 * functions below must also cope with NaNs, in particular with the policy
 * that all NaNs are equal.
 */
static bool
gbt_float4gt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return float4_gt(*((const float4 *) a), *((const float4 *) b));
}
static bool
gbt_float4ge(const void *a, const void *b, FmgrInfo *flinfo)
{
	return float4_ge(*((const float4 *) a), *((const float4 *) b));
}
static bool
gbt_float4eq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return float4_eq(*((const float4 *) a), *((const float4 *) b));
}
static bool
gbt_float4le(const void *a, const void *b, FmgrInfo *flinfo)
{
	return float4_le(*((const float4 *) a), *((const float4 *) b));
}
static bool
gbt_float4lt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return float4_lt(*((const float4 *) a), *((const float4 *) b));
}

static int
gbt_float4key_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	float4KEY  *ia = (float4KEY *) (((const Nsrt *) a)->t);
	float4KEY  *ib = (float4KEY *) (((const Nsrt *) b)->t);
	int			res;

	res = float4_cmp_internal(ia->lower, ib->lower);
	if (res != 0)
		return res;
	return float4_cmp_internal(ia->upper, ib->upper);
}

static float8
gbt_float4_dist(const void *a, const void *b, FmgrInfo *flinfo)
{
	float8		arg1 = *(const float4 *) a;
	float8		arg2 = *(const float4 *) b;
	float8		r;

	r = arg1 - arg2;
	/* needn't consider isinf case here, must be due to input infinity */
	if (unlikely(isnan(r)))
	{
		if (isnan(arg1) && isnan(arg2))
			r = 0.0;			/* treat NaNs as equal */
		else if (isnan(arg1) || isnan(arg2))
			r = get_float8_infinity();	/* max dist for NaN vs non-NaN */
		else
			r = 0.0;			/* must be Inf - Inf case */
	}
	return fabs(r);
}


static const gbtree_ninfo tinfo =
{
	gbt_t_float4,
	sizeof(float4),
	8,							/* sizeof(gbtreekey8) */
	gbt_float4gt,
	gbt_float4ge,
	gbt_float4eq,
	gbt_float4le,
	gbt_float4lt,
	gbt_float4key_cmp,
	gbt_float4_dist
};


PG_FUNCTION_INFO_V1(float4_dist);
Datum
float4_dist(PG_FUNCTION_ARGS)
{
	float4		a = PG_GETARG_FLOAT4(0);
	float4		b = PG_GETARG_FLOAT4(1);
	float4		r;

	r = a - b;
	if (unlikely(isinf(r)) && !isinf(a) && !isinf(b))
		float_overflow_error();
	if (unlikely(isnan(r)))
	{
		if (isnan(a) && isnan(b))
			r = 0.0;			/* treat NaNs as equal */
		else if (isnan(a) || isnan(b))
			r = get_float4_infinity();	/* max dist for NaN vs non-NaN */
		else
			r = 0.0;			/* must be Inf - Inf case */
	}
	PG_RETURN_FLOAT4(fabsf(r));
}


/**************************************************
 * float4 ops
 **************************************************/


Datum
gbt_float4_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_compress(entry, &tinfo));
}

Datum
gbt_float4_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_fetch(entry, &tinfo));
}

Datum
gbt_float4_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	float4		query = PG_GETARG_FLOAT4(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	float4KEY  *kkk = (float4KEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_BOOL(gbt_num_consistent(&key, (void *) &query, &strategy,
									  GIST_LEAF(entry), &tinfo,
									  fcinfo->flinfo));
}


Datum
gbt_float4_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	float4		query = PG_GETARG_FLOAT4(1);

	/* Oid		subtype = PG_GETARG_OID(3); */
	float4KEY  *kkk = (float4KEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_FLOAT8(gbt_num_distance(&key, (void *) &query, GIST_LEAF(entry),
									  &tinfo, fcinfo->flinfo));
}


Datum
gbt_float4_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(float4KEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(float4KEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo, fcinfo->flinfo));
}


Datum
gbt_float4_penalty(PG_FUNCTION_ARGS)
{
	float4KEY  *origentry = (float4KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	float4KEY  *newentry = (float4KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	float_penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);
}

Datum
gbt_float4_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit((GistEntryVector *) PG_GETARG_POINTER(0),
										(GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo, fcinfo->flinfo));
}

Datum
gbt_float4_same(PG_FUNCTION_ARGS)
{
	float4KEY  *b1 = (float4KEY *) PG_GETARG_POINTER(0);
	float4KEY  *b2 = (float4KEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}
