/*
 * contrib/btree_gist/btree_float8.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"

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
PG_FUNCTION_INFO_V1(gbt_float8_sortsupport);


static bool
gbt_float8gt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const float8 *) a) > *((const float8 *) b));
}
static bool
gbt_float8ge(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const float8 *) a) >= *((const float8 *) b));
}
static bool
gbt_float8eq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const float8 *) a) == *((const float8 *) b));
}
static bool
gbt_float8le(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const float8 *) a) <= *((const float8 *) b));
}
static bool
gbt_float8lt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const float8 *) a) < *((const float8 *) b));
}

static int
gbt_float8key_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	float8KEY  *ia = (float8KEY *) (((const Nsrt *) a)->t);
	float8KEY  *ib = (float8KEY *) (((const Nsrt *) b)->t);

	if (ia->lower == ib->lower)
	{
		if (ia->upper == ib->upper)
			return 0;

		return (ia->upper > ib->upper) ? 1 : -1;
	}

	return (ia->lower > ib->lower) ? 1 : -1;
}

static float8
gbt_float8_dist(const void *a, const void *b, FmgrInfo *flinfo)
{
	float8		arg1 = *(const float8 *) a;
	float8		arg2 = *(const float8 *) b;
	float8		r;

	r = arg1 - arg2;
	CHECKFLOATVAL(r, isinf(arg1) || isinf(arg2), true);

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
	CHECKFLOATVAL(r, isinf(a) || isinf(b), true);

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

	penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

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

static int
gbt_float8_sort_build_cmp(Datum a, Datum b, SortSupport ssup)
{
	float8KEY  *ia = (float8KEY *) DatumGetPointer(a);
	float8KEY  *ib = (float8KEY *) DatumGetPointer(b);

	/* for leaf items we expect lower == upper */
	Assert(ia->lower == ia->upper);
	Assert(ib->lower == ib->upper);

	if (ia->lower == ib->lower)
		return 0;

	return (ia->lower > ib->lower) ? 1 : -1;
}

static Datum
gbt_float8_abbrev_convert(Datum original, SortSupport ssup)
{
	float8KEY  *b1 = (float8KEY *) DatumGetPointer(original);
	float8		z = b1->lower;

#if SIZEOF_DATUM == 8
	return Float8GetDatum(z);
#else
	return Float4GetDatum((float4) z);
#endif
}

static int
gbt_float8_cmp_abbrev(Datum z1, Datum z2, SortSupport ssup)
{
#if SIZEOF_DATUM == 8
	float8		a = DatumGetFloat8(z1);
	float8		b = DatumGetFloat8(z2);
#else
	float4		a = DatumGetFloat4(z1);
	float4		b = DatumGetFloat4(z2);
#endif

	if (a > b)
		return 1;
	else if (a < b)
		return -1;
	else
		return 0;
}

static bool
gbt_float8_abbrev_abort(int memtupcount, SortSupport ssup)
{
	return false;
}

/*
 * Sort support routine for fast GiST index build by sorting.
 */
Datum
gbt_float8_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	if (ssup->abbreviate)
	{
		ssup->comparator = gbt_float8_cmp_abbrev;
		ssup->abbrev_converter = gbt_float8_abbrev_convert;
		ssup->abbrev_abort = gbt_float8_abbrev_abort;
		ssup->abbrev_full_comparator = gbt_float8_sort_build_cmp;
	}
	else
	{
		ssup->comparator = gbt_float8_sort_build_cmp;
	}
	PG_RETURN_VOID();
}
