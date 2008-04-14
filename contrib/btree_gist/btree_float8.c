#include "btree_gist.h"
#include "btree_utils_num.h"

typedef struct float8key
{
	float8		lower;
	float8		upper;
}	float8KEY;

/*
** float8 ops
*/
PG_FUNCTION_INFO_V1(gbt_float8_compress);
PG_FUNCTION_INFO_V1(gbt_float8_union);
PG_FUNCTION_INFO_V1(gbt_float8_picksplit);
PG_FUNCTION_INFO_V1(gbt_float8_consistent);
PG_FUNCTION_INFO_V1(gbt_float8_penalty);
PG_FUNCTION_INFO_V1(gbt_float8_same);

Datum		gbt_float8_compress(PG_FUNCTION_ARGS);
Datum		gbt_float8_union(PG_FUNCTION_ARGS);
Datum		gbt_float8_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_float8_consistent(PG_FUNCTION_ARGS);
Datum		gbt_float8_penalty(PG_FUNCTION_ARGS);
Datum		gbt_float8_same(PG_FUNCTION_ARGS);


static bool
gbt_float8gt(const void *a, const void *b)
{
	return (*((float8 *) a) > *((float8 *) b));
}
static bool
gbt_float8ge(const void *a, const void *b)
{
	return (*((float8 *) a) >= *((float8 *) b));
}
static bool
gbt_float8eq(const void *a, const void *b)
{
	return (*((float8 *) a) == *((float8 *) b));
}
static bool
gbt_float8le(const void *a, const void *b)
{
	return (*((float8 *) a) <= *((float8 *) b));
}
static bool
gbt_float8lt(const void *a, const void *b)
{
	return (*((float8 *) a) < *((float8 *) b));
}

static int
gbt_float8key_cmp(const void *a, const void *b)
{

	if (*(float8 *) &(((Nsrt *) a)->t[0]) > *(float8 *) &(((Nsrt *) b)->t[0]))
		return 1;
	else if (*(float8 *) &(((Nsrt *) a)->t[0]) < *(float8 *) &(((Nsrt *) b)->t[0]))
		return -1;
	return 0;

}


static const gbtree_ninfo tinfo =
{
	gbt_t_float8,
	sizeof(float8),
	gbt_float8gt,
	gbt_float8ge,
	gbt_float8eq,
	gbt_float8le,
	gbt_float8lt,
	gbt_float8key_cmp
};


/**************************************************
 * float8 ops
 **************************************************/


Datum
gbt_float8_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = NULL;

	PG_RETURN_POINTER(gbt_num_compress(retval, entry, &tinfo));
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

	key.lower = (GBT_NUMKEY *) & kkk->lower;
	key.upper = (GBT_NUMKEY *) & kkk->upper;

	PG_RETURN_BOOL(
				   gbt_num_consistent(&key, (void *) &query, &strategy, GIST_LEAF(entry), &tinfo)
		);
}


Datum
gbt_float8_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(float8KEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(float8KEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
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
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_float8_same(PG_FUNCTION_ARGS)
{
	float8KEY  *b1 = (float8KEY *) PG_GETARG_POINTER(0);
	float8KEY  *b2 = (float8KEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
