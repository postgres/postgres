#include "btree_gist.h"
#include "btree_utils_num.h"

typedef struct int32key
{
	int32		lower;
	int32		upper;
}	int32KEY;

/*
** int32 ops
*/
PG_FUNCTION_INFO_V1(gbt_int4_compress);
PG_FUNCTION_INFO_V1(gbt_int4_union);
PG_FUNCTION_INFO_V1(gbt_int4_picksplit);
PG_FUNCTION_INFO_V1(gbt_int4_consistent);
PG_FUNCTION_INFO_V1(gbt_int4_penalty);
PG_FUNCTION_INFO_V1(gbt_int4_same);

Datum		gbt_int4_compress(PG_FUNCTION_ARGS);
Datum		gbt_int4_union(PG_FUNCTION_ARGS);
Datum		gbt_int4_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_int4_consistent(PG_FUNCTION_ARGS);
Datum		gbt_int4_penalty(PG_FUNCTION_ARGS);
Datum		gbt_int4_same(PG_FUNCTION_ARGS);


static bool
gbt_int4gt(const void *a, const void *b)
{
	return (*((int32 *) a) > *((int32 *) b));
}
static bool
gbt_int4ge(const void *a, const void *b)
{
	return (*((int32 *) a) >= *((int32 *) b));
}
static bool
gbt_int4eq(const void *a, const void *b)
{
	return (*((int32 *) a) == *((int32 *) b));
}
static bool
gbt_int4le(const void *a, const void *b)
{
	return (*((int32 *) a) <= *((int32 *) b));
}
static bool
gbt_int4lt(const void *a, const void *b)
{
	return (*((int32 *) a) < *((int32 *) b));
}

static int
gbt_int4key_cmp(const void *a, const void *b)
{

	if (*(int32 *) &(((Nsrt *) a)->t[0]) > *(int32 *) &(((Nsrt *) b)->t[0]))
		return 1;
	else if (*(int32 *) &(((Nsrt *) a)->t[0]) < *(int32 *) &(((Nsrt *) b)->t[0]))
		return -1;
	return 0;

}


static const gbtree_ninfo tinfo =
{
	gbt_t_int4,
	sizeof(int32),
	gbt_int4gt,
	gbt_int4ge,
	gbt_int4eq,
	gbt_int4le,
	gbt_int4lt,
	gbt_int4key_cmp
};


/**************************************************
 * int32 ops
 **************************************************/


Datum
gbt_int4_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = NULL;

	PG_RETURN_POINTER(gbt_num_compress(retval, entry, &tinfo));
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

	key.lower = (GBT_NUMKEY *) & kkk->lower;
	key.upper = (GBT_NUMKEY *) & kkk->upper;

	PG_RETURN_BOOL(
				   gbt_num_consistent(&key, (void *) &query, &strategy, GIST_LEAF(entry), &tinfo)
		);
}


Datum
gbt_int4_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(int32KEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(int32KEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
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
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_int4_same(PG_FUNCTION_ARGS)
{
	int32KEY   *b1 = (int32KEY *) PG_GETARG_POINTER(0);
	int32KEY   *b2 = (int32KEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
