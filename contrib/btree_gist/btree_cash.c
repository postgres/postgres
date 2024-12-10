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

	ra = i64abs(r);

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

	PG_RETURN_BOOL(gbt_num_consistent(&key, &query, &strategy,
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

	PG_RETURN_FLOAT8(gbt_num_distance(&key, &query, GIST_LEAF(entry),
									  &tinfo, fcinfo->flinfo));
}


Datum
gbt_cash_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(cashKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(cashKEY);
	PG_RETURN_POINTER(gbt_num_union(out, entryvec, &tinfo, fcinfo->flinfo));
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
