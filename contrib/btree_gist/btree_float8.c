/*
 * contrib/btree_gist/btree_float8.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/float.h"

typedef struct float8key
{
	float8		lower;
	float8		upper;
} float8KEY;

/*
** float8 ops
*/
PG_FUNCTION_INFO_V1(gbt_float8_compress);
PG_FUNCTION_INFO_V1(gbt_float8_fetch);
PG_FUNCTION_INFO_V1(gbt_float8_union);
PG_FUNCTION_INFO_V1(gbt_float8_picksplit);
PG_FUNCTION_INFO_V1(gbt_float8_consistent);
PG_FUNCTION_INFO_V1(gbt_float8_distance);
PG_FUNCTION_INFO_V1(gbt_float8_penalty);
PG_FUNCTION_INFO_V1(gbt_float8_same);


/*
 * Use the NaN-aware comparators from utils/float.h, so that our results
 * will agree with standard btree indexes.  Note that penalty and distance
 * functions below must also cope with NaNs, in particular with the policy
 * that all NaNs are equal.
 */
static bool
gbt_float8gt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return float8_gt(*((const float8 *) a), *((const float8 *) b));
}
static bool
gbt_float8ge(const void *a, const void *b, FmgrInfo *flinfo)
{
	return float8_ge(*((const float8 *) a), *((const float8 *) b));
}
static bool
gbt_float8eq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return float8_eq(*((const float8 *) a), *((const float8 *) b));
}
static bool
gbt_float8le(const void *a, const void *b, FmgrInfo *flinfo)
{
	return float8_le(*((const float8 *) a), *((const float8 *) b));
}
static bool
gbt_float8lt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return float8_lt(*((const float8 *) a), *((const float8 *) b));
}

static int
gbt_float8key_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	float8KEY  *ia = (float8KEY *) (((const Nsrt *) a)->t);
	float8KEY  *ib = (float8KEY *) (((const Nsrt *) b)->t);
	int			res;

	res = float8_cmp_internal(ia->lower, ib->lower);
	if (res != 0)
		return res;
	return float8_cmp_internal(ia->upper, ib->upper);
}

static float8
gbt_float8_dist(const void *a, const void *b, FmgrInfo *flinfo)
{
	float8		arg1 = *(const float8 *) a;
	float8		arg2 = *(const float8 *) b;
	float8		r;

	r = arg1 - arg2;
	if (unlikely(isinf(r)) && !isinf(arg1) && !isinf(arg2))
		float_overflow_error();
	if (unlikely(isnan(r)))
	{
		if (isnan(arg1) && isnan(arg2))
			r = 0.0;			/* treat NaNs as equal */
		else if (isnan(arg1) || isnan(arg2))
			r = get_float8_infinity();	/* max dist for NaN vs non-NaN */
		else
			r = 0.0;			/* must be Inf - Inf case */
	}
	return Abs(r);
}


static const gbtree_ninfo tinfo =
{
	gbt_t_float8,
	sizeof(float8),
	16,							/* sizeof(gbtreekey16) */
	gbt_float8gt,
	gbt_float8ge,
	gbt_float8eq,
	gbt_float8le,
	gbt_float8lt,
	gbt_float8key_cmp,
	gbt_float8_dist
};


PG_FUNCTION_INFO_V1(float8_dist);
Datum
float8_dist(PG_FUNCTION_ARGS)
{
	float8		a = PG_GETARG_FLOAT8(0);
	float8		b = PG_GETARG_FLOAT8(1);
	float8		r;

	r = a - b;
	if (unlikely(isinf(r)) && !isinf(a) && !isinf(b))
		float_overflow_error();
	if (unlikely(isnan(r)))
	{
		if (isnan(a) && isnan(b))
			r = 0.0;			/* treat NaNs as equal */
		else if (isnan(a) || isnan(b))
			r = get_float8_infinity();	/* max dist for NaN vs non-NaN */
		else
			r = 0.0;			/* must be Inf - Inf case */
	}
	PG_RETURN_FLOAT8(Abs(r));
}

/**************************************************
 * float8 ops
 **************************************************/


Datum
gbt_float8_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_compress(entry, &tinfo));
}

Datum
gbt_float8_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_fetch(entry, &tinfo));
}

Datum
gbt_float8_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	float8		query = PG_GETARG_FLOAT8(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	float8KEY  *kkk = (float8KEY *) DatumGetPointer(entry->key);
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
gbt_float8_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	float8		query = PG_GETARG_FLOAT8(1);

	/* Oid		subtype = PG_GETARG_OID(3); */
	float8KEY  *kkk = (float8KEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_FLOAT8(gbt_num_distance(&key, (void *) &query, GIST_LEAF(entry),
									  &tinfo, fcinfo->flinfo));
}


Datum
gbt_float8_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(float8KEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(float8KEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo, fcinfo->flinfo));
}


Datum
gbt_float8_penalty(PG_FUNCTION_ARGS)
{
	float8KEY  *origentry = (float8KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	float8KEY  *newentry = (float8KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	float_penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);
}

Datum
gbt_float8_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit((GistEntryVector *) PG_GETARG_POINTER(0),
										(GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo, fcinfo->flinfo));
}

Datum
gbt_float8_same(PG_FUNCTION_ARGS)
{
	float8KEY  *b1 = (float8KEY *) PG_GETARG_POINTER(0);
	float8KEY  *b2 = (float8KEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}
