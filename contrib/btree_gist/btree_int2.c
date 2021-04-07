/*
 * contrib/btree_gist/btree_int2.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "common/int.h"

typedef struct int16key
{
	int16		lower;
	int16		upper;
} int16KEY;

/*
** int16 ops
*/
PG_FUNCTION_INFO_V1(gbt_int2_compress);
PG_FUNCTION_INFO_V1(gbt_int2_fetch);
PG_FUNCTION_INFO_V1(gbt_int2_union);
PG_FUNCTION_INFO_V1(gbt_int2_picksplit);
PG_FUNCTION_INFO_V1(gbt_int2_consistent);
PG_FUNCTION_INFO_V1(gbt_int2_distance);
PG_FUNCTION_INFO_V1(gbt_int2_penalty);
PG_FUNCTION_INFO_V1(gbt_int2_same);
PG_FUNCTION_INFO_V1(gbt_int2_sortsupport);

static bool
gbt_int2gt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int16 *) a) > *((const int16 *) b));
}
static bool
gbt_int2ge(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int16 *) a) >= *((const int16 *) b));
}
static bool
gbt_int2eq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int16 *) a) == *((const int16 *) b));
}
static bool
gbt_int2le(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int16 *) a) <= *((const int16 *) b));
}
static bool
gbt_int2lt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int16 *) a) < *((const int16 *) b));
}

static int
gbt_int2key_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	int16KEY   *ia = (int16KEY *) (((const Nsrt *) a)->t);
	int16KEY   *ib = (int16KEY *) (((const Nsrt *) b)->t);

	if (ia->lower == ib->lower)
	{
		if (ia->upper == ib->upper)
			return 0;

		return (ia->upper > ib->upper) ? 1 : -1;
	}

	return (ia->lower > ib->lower) ? 1 : -1;
}

static float8
gbt_int2_dist(const void *a, const void *b, FmgrInfo *flinfo)
{
	return GET_FLOAT_DISTANCE(int16, a, b);
}


static const gbtree_ninfo tinfo =
{
	gbt_t_int2,
	sizeof(int16),
	4,							/* sizeof(gbtreekey4) */
	gbt_int2gt,
	gbt_int2ge,
	gbt_int2eq,
	gbt_int2le,
	gbt_int2lt,
	gbt_int2key_cmp,
	gbt_int2_dist
};


PG_FUNCTION_INFO_V1(int2_dist);
Datum
int2_dist(PG_FUNCTION_ARGS)
{
	int16		a = PG_GETARG_INT16(0);
	int16		b = PG_GETARG_INT16(1);
	int16		r;
	int16		ra;

	if (pg_sub_s16_overflow(a, b, &r) ||
		r == PG_INT16_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	ra = Abs(r);

	PG_RETURN_INT16(ra);
}


/**************************************************
 * int16 ops
 **************************************************/


Datum
gbt_int2_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_compress(entry, &tinfo));
}

Datum
gbt_int2_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_fetch(entry, &tinfo));
}

Datum
gbt_int2_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	int16		query = PG_GETARG_INT16(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	int16KEY   *kkk = (int16KEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_BOOL(gbt_num_consistent(&key, (void *) &query, &strategy,
									  GIST_LEAF(entry), &tinfo, fcinfo->flinfo));
}


Datum
gbt_int2_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	int16		query = PG_GETARG_INT16(1);

	/* Oid		subtype = PG_GETARG_OID(3); */
	int16KEY   *kkk = (int16KEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_FLOAT8(gbt_num_distance(&key, (void *) &query, GIST_LEAF(entry),
									  &tinfo, fcinfo->flinfo));
}


Datum
gbt_int2_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(int16KEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(int16KEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo, fcinfo->flinfo));
}


Datum
gbt_int2_penalty(PG_FUNCTION_ARGS)
{
	int16KEY   *origentry = (int16KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	int16KEY   *newentry = (int16KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);
}

Datum
gbt_int2_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit((GistEntryVector *) PG_GETARG_POINTER(0),
										(GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo, fcinfo->flinfo));
}

Datum
gbt_int2_same(PG_FUNCTION_ARGS)
{
	int16KEY   *b1 = (int16KEY *) PG_GETARG_POINTER(0);
	int16KEY   *b2 = (int16KEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}

static int
gbt_int2_sort_build_cmp(Datum a, Datum b, SortSupport ssup)
{
	int16KEY   *ia = (int16KEY *) DatumGetPointer(a);
	int16KEY   *ib = (int16KEY *) DatumGetPointer(b);

	/* for leaf items we expect lower == upper */
	Assert(ia->lower == ia->upper);
	Assert(ib->lower == ib->upper);

	if (ia->lower == ib->lower)
		return 0;

	return (ia->lower > ib->lower) ? 1 : -1;
}

static Datum
gbt_int2_abbrev_convert(Datum original, SortSupport ssup)
{
	int16KEY   *b1 = (int16KEY *) DatumGetPointer(original);

	return Int16GetDatum(b1->lower);
}

static int
gbt_int2_cmp_abbrev(Datum z1, Datum z2, SortSupport ssup)
{
	int16		a = DatumGetInt16(z1);
	int16		b = DatumGetInt16(z2);

	if (a > b)
		return 1;
	else if (a < b)
		return -1;
	else
		return 0;
}

/*
 * We never consider aborting the abbreviation.
 */
static bool
gbt_int2_abbrev_abort(int memtupcount, SortSupport ssup)
{
	return false;
}

/*
 * Sort support routine for fast GiST index build by sorting.
 */
Datum
gbt_int2_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	if (ssup->abbreviate)
	{
		ssup->comparator = gbt_int2_cmp_abbrev;
		ssup->abbrev_converter = gbt_int2_abbrev_convert;
		ssup->abbrev_abort = gbt_int2_abbrev_abort;
		ssup->abbrev_full_comparator = gbt_int2_sort_build_cmp;
	}
	else
	{
		ssup->comparator = gbt_int2_sort_build_cmp;
	}
	PG_RETURN_VOID();
}
