/*
 * contrib/btree_gist/btree_int8.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "common/int.h"
#include "utils/sortsupport.h"

typedef struct int64key
{
	int64		lower;
	int64		upper;
} int64KEY;

/* GiST support functions */
PG_FUNCTION_INFO_V1(gbt_int8_compress);
PG_FUNCTION_INFO_V1(gbt_int8_fetch);
PG_FUNCTION_INFO_V1(gbt_int8_union);
PG_FUNCTION_INFO_V1(gbt_int8_picksplit);
PG_FUNCTION_INFO_V1(gbt_int8_consistent);
PG_FUNCTION_INFO_V1(gbt_int8_distance);
PG_FUNCTION_INFO_V1(gbt_int8_penalty);
PG_FUNCTION_INFO_V1(gbt_int8_same);
PG_FUNCTION_INFO_V1(gbt_int8_sortsupport);


static bool
gbt_int8gt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int64 *) a) > *((const int64 *) b));
}
static bool
gbt_int8ge(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int64 *) a) >= *((const int64 *) b));
}
static bool
gbt_int8eq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int64 *) a) == *((const int64 *) b));
}
static bool
gbt_int8le(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int64 *) a) <= *((const int64 *) b));
}
static bool
gbt_int8lt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int64 *) a) < *((const int64 *) b));
}

static int
gbt_int8key_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	int64KEY   *ia = (int64KEY *) (((const Nsrt *) a)->t);
	int64KEY   *ib = (int64KEY *) (((const Nsrt *) b)->t);

	if (ia->lower == ib->lower)
	{
		if (ia->upper == ib->upper)
			return 0;

		return (ia->upper > ib->upper) ? 1 : -1;
	}

	return (ia->lower > ib->lower) ? 1 : -1;
}

static float8
gbt_int8_dist(const void *a, const void *b, FmgrInfo *flinfo)
{
	return GET_FLOAT_DISTANCE(int64, a, b);
}


static const gbtree_ninfo tinfo =
{
	gbt_t_int8,
	sizeof(int64),
	16,							/* sizeof(gbtreekey16) */
	gbt_int8gt,
	gbt_int8ge,
	gbt_int8eq,
	gbt_int8le,
	gbt_int8lt,
	gbt_int8key_cmp,
	gbt_int8_dist
};


PG_FUNCTION_INFO_V1(int8_dist);
Datum
int8_dist(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int64		b = PG_GETARG_INT64(1);
	int64		r;
	int64		ra;

	if (pg_sub_s64_overflow(a, b, &r) ||
		r == PG_INT64_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	ra = i64abs(r);

	PG_RETURN_INT64(ra);
}


/**************************************************
 * GiST support functions
 **************************************************/

Datum
gbt_int8_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_compress(entry, &tinfo));
}

Datum
gbt_int8_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_fetch(entry, &tinfo));
}

Datum
gbt_int8_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	int64		query = PG_GETARG_INT64(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	int64KEY   *kkk = (int64KEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_BOOL(gbt_num_consistent(&key, &query, &strategy,
									  GIST_LEAF(entry), &tinfo, fcinfo->flinfo));
}

Datum
gbt_int8_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	int64		query = PG_GETARG_INT64(1);

	/* Oid		subtype = PG_GETARG_OID(3); */
	int64KEY   *kkk = (int64KEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_FLOAT8(gbt_num_distance(&key, &query, GIST_LEAF(entry),
									  &tinfo, fcinfo->flinfo));
}

Datum
gbt_int8_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(int64KEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(int64KEY);
	PG_RETURN_POINTER(gbt_num_union(out, entryvec, &tinfo, fcinfo->flinfo));
}

Datum
gbt_int8_penalty(PG_FUNCTION_ARGS)
{
	int64KEY   *origentry = (int64KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	int64KEY   *newentry = (int64KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);
}

Datum
gbt_int8_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit((GistEntryVector *) PG_GETARG_POINTER(0),
										(GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo, fcinfo->flinfo));
}

Datum
gbt_int8_same(PG_FUNCTION_ARGS)
{
	int64KEY   *b1 = (int64KEY *) PG_GETARG_POINTER(0);
	int64KEY   *b2 = (int64KEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}

static int
gbt_int8_ssup_cmp(Datum x, Datum y, SortSupport ssup)
{
	int64KEY   *arg1 = (int64KEY *) DatumGetPointer(x);
	int64KEY   *arg2 = (int64KEY *) DatumGetPointer(y);

	/* for leaf items we expect lower == upper, so only compare lower */
	if (arg1->lower < arg2->lower)
		return -1;
	else if (arg1->lower > arg2->lower)
		return 1;
	else
		return 0;

}

Datum
gbt_int8_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = gbt_int8_ssup_cmp;
	PG_RETURN_VOID();
}
