/*
 * contrib/btree_gist/btree_int4.c
 */
#include "postgres.h"
#include "btree_gist.h"
#include "btree_utils_num.h"
#include "common/int.h"
#include "utils/sortsupport.h"

typedef struct int32key
{
	int32		lower;
	int32		upper;
} int32KEY;

/* GiST support functions */
PG_FUNCTION_INFO_V1(gbt_int4_compress);
PG_FUNCTION_INFO_V1(gbt_int4_fetch);
PG_FUNCTION_INFO_V1(gbt_int4_union);
PG_FUNCTION_INFO_V1(gbt_int4_picksplit);
PG_FUNCTION_INFO_V1(gbt_int4_consistent);
PG_FUNCTION_INFO_V1(gbt_int4_distance);
PG_FUNCTION_INFO_V1(gbt_int4_penalty);
PG_FUNCTION_INFO_V1(gbt_int4_same);
PG_FUNCTION_INFO_V1(gbt_int4_sortsupport);

static bool
gbt_int4gt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int32 *) a) > *((const int32 *) b));
}
static bool
gbt_int4ge(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int32 *) a) >= *((const int32 *) b));
}
static bool
gbt_int4eq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int32 *) a) == *((const int32 *) b));
}
static bool
gbt_int4le(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int32 *) a) <= *((const int32 *) b));
}
static bool
gbt_int4lt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int32 *) a) < *((const int32 *) b));
}

static int
gbt_int4key_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	int32KEY   *ia = (int32KEY *) (((const Nsrt *) a)->t);
	int32KEY   *ib = (int32KEY *) (((const Nsrt *) b)->t);

	if (ia->lower == ib->lower)
	{
		if (ia->upper == ib->upper)
			return 0;

		return (ia->upper > ib->upper) ? 1 : -1;
	}

	return (ia->lower > ib->lower) ? 1 : -1;
}

static float8
gbt_int4_dist(const void *a, const void *b, FmgrInfo *flinfo)
{
	return GET_FLOAT_DISTANCE(int32, a, b);
}


static const gbtree_ninfo tinfo =
{
	gbt_t_int4,
	sizeof(int32),
	8,							/* sizeof(gbtreekey8) */
	gbt_int4gt,
	gbt_int4ge,
	gbt_int4eq,
	gbt_int4le,
	gbt_int4lt,
	gbt_int4key_cmp,
	gbt_int4_dist
};


PG_FUNCTION_INFO_V1(int4_dist);
Datum
int4_dist(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int32		b = PG_GETARG_INT32(1);
	int32		r;
	int32		ra;

	if (pg_sub_s32_overflow(a, b, &r) ||
		r == PG_INT32_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	ra = abs(r);

	PG_RETURN_INT32(ra);
}


/**************************************************
 * GiST support functions
 **************************************************/

Datum
gbt_int4_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_compress(entry, &tinfo));
}

Datum
gbt_int4_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_fetch(entry, &tinfo));
}

Datum
gbt_int4_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	int32		query = PG_GETARG_INT32(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	int32KEY   *kkk = (int32KEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_BOOL(gbt_num_consistent(&key, &query, &strategy,
									  GIST_LEAF(entry), &tinfo, fcinfo->flinfo));
}

Datum
gbt_int4_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	int32		query = PG_GETARG_INT32(1);

	/* Oid		subtype = PG_GETARG_OID(3); */
	int32KEY   *kkk = (int32KEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_FLOAT8(gbt_num_distance(&key, &query, GIST_LEAF(entry),
									  &tinfo, fcinfo->flinfo));
}

Datum
gbt_int4_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(int32KEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(int32KEY);
	PG_RETURN_POINTER(gbt_num_union(out, entryvec, &tinfo, fcinfo->flinfo));
}

Datum
gbt_int4_penalty(PG_FUNCTION_ARGS)
{
	int32KEY   *origentry = (int32KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	int32KEY   *newentry = (int32KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);
}

Datum
gbt_int4_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit((GistEntryVector *) PG_GETARG_POINTER(0),
										(GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo, fcinfo->flinfo));
}

Datum
gbt_int4_same(PG_FUNCTION_ARGS)
{
	int32KEY   *b1 = (int32KEY *) PG_GETARG_POINTER(0);
	int32KEY   *b2 = (int32KEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}

static int
gbt_int4_ssup_cmp(Datum a, Datum b, SortSupport ssup)
{
	int32KEY   *ia = (int32KEY *) DatumGetPointer(a);
	int32KEY   *ib = (int32KEY *) DatumGetPointer(b);

	/* for leaf items we expect lower == upper, so only compare lower */
	if (ia->lower < ib->lower)
		return -1;
	else if (ia->lower > ib->lower)
		return 1;
	else
		return 0;
}

Datum
gbt_int4_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = gbt_int4_ssup_cmp;
	PG_RETURN_VOID();
}
