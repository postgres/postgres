#include "btree_gist.h"
#include "btree_utils_num.h"

typedef struct int64key
{
	int64		lower;
	int64		upper;
}	int64KEY;

/*
** int64 ops
*/
PG_FUNCTION_INFO_V1(gbt_int8_compress);
PG_FUNCTION_INFO_V1(gbt_int8_union);
PG_FUNCTION_INFO_V1(gbt_int8_picksplit);
PG_FUNCTION_INFO_V1(gbt_int8_consistent);
PG_FUNCTION_INFO_V1(gbt_int8_penalty);
PG_FUNCTION_INFO_V1(gbt_int8_same);

Datum		gbt_int8_compress(PG_FUNCTION_ARGS);
Datum		gbt_int8_union(PG_FUNCTION_ARGS);
Datum		gbt_int8_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_int8_consistent(PG_FUNCTION_ARGS);
Datum		gbt_int8_penalty(PG_FUNCTION_ARGS);
Datum		gbt_int8_same(PG_FUNCTION_ARGS);


static bool
gbt_int8gt(const void *a, const void *b)
{
	return (*((int64 *) a) > *((int64 *) b));
}
static bool
gbt_int8ge(const void *a, const void *b)
{
	return (*((int64 *) a) >= *((int64 *) b));
}
static bool
gbt_int8eq(const void *a, const void *b)
{
	return (*((int64 *) a) == *((int64 *) b));
}
static bool
gbt_int8le(const void *a, const void *b)
{
	return (*((int64 *) a) <= *((int64 *) b));
}
static bool
gbt_int8lt(const void *a, const void *b)
{
	return (*((int64 *) a) < *((int64 *) b));
}

static int
gbt_int8key_cmp(const void *a, const void *b)
{

	if (*(int64 *) &(((Nsrt *) a)->t[0]) > *(int64 *) &(((Nsrt *) b)->t[0]))
		return 1;
	else if (*(int64 *) &(((Nsrt *) a)->t[0]) < *(int64 *) &(((Nsrt *) b)->t[0]))
		return -1;
	return 0;

}


static const gbtree_ninfo tinfo =
{
	gbt_t_int8,
	sizeof(int64),
	gbt_int8gt,
	gbt_int8ge,
	gbt_int8eq,
	gbt_int8le,
	gbt_int8lt,
	gbt_int8key_cmp
};


/**************************************************
 * int64 ops
 **************************************************/


Datum
gbt_int8_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = NULL;

	PG_RETURN_POINTER(gbt_num_compress(retval, entry, &tinfo));
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

	key.lower = (GBT_NUMKEY *) & kkk->lower;
	key.upper = (GBT_NUMKEY *) & kkk->upper;

	PG_RETURN_BOOL(
				   gbt_num_consistent(&key, (void *) &query, &strategy, GIST_LEAF(entry), &tinfo)
		);
}


Datum
gbt_int8_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(int64KEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(int64KEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
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
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_int8_same(PG_FUNCTION_ARGS)
{
	int64KEY   *b1 = (int64KEY *) PG_GETARG_POINTER(0);
	int64KEY   *b2 = (int64KEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
