#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/cash.h"

typedef struct
{
	Cash		lower;
	Cash		upper;
}	cashKEY;

/*
** Cash ops
*/
PG_FUNCTION_INFO_V1(gbt_cash_compress);
PG_FUNCTION_INFO_V1(gbt_cash_union);
PG_FUNCTION_INFO_V1(gbt_cash_picksplit);
PG_FUNCTION_INFO_V1(gbt_cash_consistent);
PG_FUNCTION_INFO_V1(gbt_cash_penalty);
PG_FUNCTION_INFO_V1(gbt_cash_same);

Datum		gbt_cash_compress(PG_FUNCTION_ARGS);
Datum		gbt_cash_union(PG_FUNCTION_ARGS);
Datum		gbt_cash_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_cash_consistent(PG_FUNCTION_ARGS);
Datum		gbt_cash_penalty(PG_FUNCTION_ARGS);
Datum		gbt_cash_same(PG_FUNCTION_ARGS);

static bool
gbt_cashgt(const void *a, const void *b)
{
	return (*((Cash *) a) > *((Cash *) b));
}
static bool
gbt_cashge(const void *a, const void *b)
{
	return (*((Cash *) a) >= *((Cash *) b));
}
static bool
gbt_casheq(const void *a, const void *b)
{
	return (*((Cash *) a) == *((Cash *) b));
}
static bool
gbt_cashle(const void *a, const void *b)
{
	return (*((Cash *) a) <= *((Cash *) b));
}
static bool
gbt_cashlt(const void *a, const void *b)
{
	return (*((Cash *) a) < *((Cash *) b));
}

static int
gbt_cashkey_cmp(const void *a, const void *b)
{

	if (*(Cash *) &(((Nsrt *) a)->t[0]) > *(Cash *) &(((Nsrt *) b)->t[0]))
		return 1;
	else if (*(Cash *) &(((Nsrt *) a)->t[0]) < *(Cash *) &(((Nsrt *) b)->t[0]))
		return -1;
	return 0;

}


static const gbtree_ninfo tinfo =
{
	gbt_t_cash,
	sizeof(Cash),
	gbt_cashgt,
	gbt_cashge,
	gbt_casheq,
	gbt_cashle,
	gbt_cashlt,
	gbt_cashkey_cmp
};


/**************************************************
 * Cash ops
 **************************************************/


Datum
gbt_cash_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = NULL;

	PG_RETURN_POINTER(gbt_num_compress(retval, entry, &tinfo));
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

	key.lower = (GBT_NUMKEY *) & kkk->lower;
	key.upper = (GBT_NUMKEY *) & kkk->upper;

	PG_RETURN_BOOL(
				   gbt_num_consistent(&key, (void *) &query, &strategy, GIST_LEAF(entry), &tinfo)
		);
}


Datum
gbt_cash_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(cashKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(cashKEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
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
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_cash_same(PG_FUNCTION_ARGS)
{
	cashKEY    *b1 = (cashKEY *) PG_GETARG_POINTER(0);
	cashKEY    *b2 = (cashKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
