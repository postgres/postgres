/*
 * contrib/btree_gist/btree_cash.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "common/int.h"
#include "utils/cash.h"

typedef struct
{
	Cash		lower;
	Cash		upper;
} cashKEY;

/*
** Cash ops
*/
PG_FUNCTION_INFO_V1(gbt_cash_compress);
PG_FUNCTION_INFO_V1(gbt_cash_fetch);
PG_FUNCTION_INFO_V1(gbt_cash_union);
PG_FUNCTION_INFO_V1(gbt_cash_picksplit);
PG_FUNCTION_INFO_V1(gbt_cash_consistent);
PG_FUNCTION_INFO_V1(gbt_cash_distance);
PG_FUNCTION_INFO_V1(gbt_cash_penalty);
PG_FUNCTION_INFO_V1(gbt_cash_same);
PG_FUNCTION_INFO_V1(gbt_cash_sortsupport);

static bool
gbt_cashgt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const Cash *) a) > *((const Cash *) b));
}
static bool
gbt_cashge(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const Cash *) a) >= *((const Cash *) b));
}
static bool
gbt_casheq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const Cash *) a) == *((const Cash *) b));
}
static bool
gbt_cashle(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const Cash *) a) <= *((const Cash *) b));
}
static bool
gbt_cashlt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const Cash *) a) < *((const Cash *) b));
}

static int
gbt_cashkey_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	cashKEY    *ia = (cashKEY *) (((const Nsrt *) a)->t);
	cashKEY    *ib = (cashKEY *) (((const Nsrt *) b)->t);

	if (ia->lower == ib->lower)
	{
		if (ia->upper == ib->upper)
			return 0;

		return (ia->upper > ib->upper) ? 1 : -1;
	}

	return (ia->lower > ib->lower) ? 1 : -1;
}

static float8
gbt_cash_dist(const void *a, const void *b, FmgrInfo *flinfo)
{
	return GET_FLOAT_DISTANCE(Cash, a, b);
}


static const gbtree_ninfo tinfo =
{
	gbt_t_cash,
	sizeof(Cash),
	16,							/* sizeof(gbtreekey16) */
	gbt_cashgt,
	gbt_cashge,
	gbt_casheq,
	gbt_cashle,
	gbt_cashlt,
	gbt_cashkey_cmp,
	gbt_cash_dist
};


PG_FUNCTION_INFO_V1(cash_dist);
Datum
cash_dist(PG_FUNCTION_ARGS)
{
	Cash		a = PG_GETARG_CASH(0);
	Cash		b = PG_GETARG_CASH(1);
	Cash		r;
	Cash		ra;

	if (pg_sub_s64_overflow(a, b, &r) ||
		r == PG_INT64_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("money out of range")));

	ra = Abs(r);

	PG_RETURN_CASH(ra);
}

/**************************************************
 * Cash ops
 **************************************************/


Datum
gbt_cash_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_compress(entry, &tinfo));
}

Datum
gbt_cash_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_fetch(entry, &tinfo));
}

Datum
gbt_cash_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Cash		query = PG_GETARG_CASH(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	cashKEY    *kkk = (cashKEY *) DatumGetPointer(entry->key);
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
gbt_cash_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Cash		query = PG_GETARG_CASH(1);

	/* Oid		subtype = PG_GETARG_OID(3); */
	cashKEY    *kkk = (cashKEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_FLOAT8(gbt_num_distance(&key, (void *) &query, GIST_LEAF(entry),
									  &tinfo, fcinfo->flinfo));
}


Datum
gbt_cash_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(cashKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(cashKEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo, fcinfo->flinfo));
}


Datum
gbt_cash_penalty(PG_FUNCTION_ARGS)
{
	cashKEY    *origentry = (cashKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	cashKEY    *newentry = (cashKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);

}

Datum
gbt_cash_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit((GistEntryVector *) PG_GETARG_POINTER(0),
										(GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo, fcinfo->flinfo));
}

Datum
gbt_cash_same(PG_FUNCTION_ARGS)
{
	cashKEY    *b1 = (cashKEY *) PG_GETARG_POINTER(0);
	cashKEY    *b2 = (cashKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}

static int
gbt_cash_sort_build_cmp(Datum a, Datum b, SortSupport ssup)
{
	cashKEY    *ia = (cashKEY *) DatumGetPointer(a);
	cashKEY    *ib = (cashKEY *) DatumGetPointer(b);

	/* for leaf items we expect lower == upper */
	Assert(ia->lower == ia->upper);
	Assert(ib->lower == ib->upper);

	if (ia->lower == ib->lower)
		return 0;

	return (ia->lower > ib->lower) ? 1 : -1;
}

static Datum
gbt_cash_abbrev_convert(Datum original, SortSupport ssup)
{
	cashKEY    *b1 = (cashKEY *) DatumGetPointer(original);
	int64		z = b1->lower;

#if SIZEOF_DATUM == 8
	return Int64GetDatum(z);
#else
	return Int32GetDatum(z >> 32);
#endif
}

static int
gbt_cash_cmp_abbrev(Datum z1, Datum z2, SortSupport ssup)
{
#if SIZEOF_DATUM == 8
	int64		a = DatumGetInt64(z1);
	int64		b = DatumGetInt64(z2);
#else
	int32		a = DatumGetInt32(z1);
	int32		b = DatumGetInt32(z2);
#endif

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
gbt_cash_abbrev_abort(int memtupcount, SortSupport ssup)
{
	return false;
}

/*
 * Sort support routine for fast GiST index build by sorting.
 */
Datum
gbt_cash_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	if (ssup->abbreviate)
	{
		ssup->comparator = gbt_cash_cmp_abbrev;
		ssup->abbrev_converter = gbt_cash_abbrev_convert;
		ssup->abbrev_abort = gbt_cash_abbrev_abort;
		ssup->abbrev_full_comparator = gbt_cash_sort_build_cmp;
	}
	else
	{
		ssup->comparator = gbt_cash_sort_build_cmp;
	}
	PG_RETURN_VOID();
}
