/*
 * contrib/btree_gist/btree_bool.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/sortsupport.h"

typedef struct boolkey
{
	bool		lower;
	bool		upper;
} boolKEY;

/* GiST support functions */
PG_FUNCTION_INFO_V1(gbt_bool_compress);
PG_FUNCTION_INFO_V1(gbt_bool_fetch);
PG_FUNCTION_INFO_V1(gbt_bool_union);
PG_FUNCTION_INFO_V1(gbt_bool_picksplit);
PG_FUNCTION_INFO_V1(gbt_bool_consistent);
PG_FUNCTION_INFO_V1(gbt_bool_penalty);
PG_FUNCTION_INFO_V1(gbt_bool_same);
PG_FUNCTION_INFO_V1(gbt_bool_sortsupport);

static bool
gbt_boolgt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const bool *) a) > *((const bool *) b));
}
static bool
gbt_boolge(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const bool *) a) >= *((const bool *) b));
}
static bool
gbt_booleq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const bool *) a) == *((const bool *) b));
}
static bool
gbt_boolle(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const bool *) a) <= *((const bool *) b));
}
static bool
gbt_boollt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const bool *) a) < *((const bool *) b));
}

static int
gbt_boolkey_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	boolKEY    *ia = (boolKEY *) (((const Nsrt *) a)->t);
	boolKEY    *ib = (boolKEY *) (((const Nsrt *) b)->t);

	if (ia->lower == ib->lower)
	{
		if (ia->upper == ib->upper)
			return 0;

		return (ia->upper > ib->upper) ? 1 : -1;
	}

	return (ia->lower > ib->lower) ? 1 : -1;
}


static const gbtree_ninfo tinfo =
{
	gbt_t_bool,
	sizeof(bool),
	2,							/* sizeof(gbtreekey2) */
	gbt_boolgt,
	gbt_boolge,
	gbt_booleq,
	gbt_boolle,
	gbt_boollt,
	gbt_boolkey_cmp,
};


/**************************************************
 * GiST support functions
 **************************************************/

Datum
gbt_bool_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_compress(entry, &tinfo));
}

Datum
gbt_bool_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_fetch(entry, &tinfo));
}

Datum
gbt_bool_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	bool		query = PG_GETARG_INT16(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	boolKEY    *kkk = (boolKEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_BOOL(gbt_num_consistent(&key, &query, &strategy,
									  GIST_LEAF(entry), &tinfo, fcinfo->flinfo));
}

Datum
gbt_bool_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(boolKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(boolKEY);
	PG_RETURN_POINTER(gbt_num_union(out, entryvec, &tinfo, fcinfo->flinfo));
}

Datum
gbt_bool_penalty(PG_FUNCTION_ARGS)
{
	boolKEY    *origentry = (boolKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	boolKEY    *newentry = (boolKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);
}

Datum
gbt_bool_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit((GistEntryVector *) PG_GETARG_POINTER(0),
										(GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo, fcinfo->flinfo));
}

Datum
gbt_bool_same(PG_FUNCTION_ARGS)
{
	boolKEY    *b1 = (boolKEY *) PG_GETARG_POINTER(0);
	boolKEY    *b2 = (boolKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}

static int
gbt_bool_ssup_cmp(Datum x, Datum y, SortSupport ssup)
{
	boolKEY    *arg1 = (boolKEY *) DatumGetPointer(x);
	boolKEY    *arg2 = (boolKEY *) DatumGetPointer(y);

	/* for leaf items we expect lower == upper, so only compare lower */
	return (int32) arg1->lower - (int32) arg2->lower;
}

Datum
gbt_bool_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = gbt_bool_ssup_cmp;
	ssup->ssup_extra = NULL;

	PG_RETURN_VOID();
}
