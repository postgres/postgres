/*
 * $PostgreSQL: pgsql/contrib/btree_gist/btree_float4.c,v 1.9 2009/12/02 13:13:24 teodor Exp $
 */
#include "btree_gist.h"
#include "btree_utils_num.h"

typedef struct float4key
{
	float4		lower;
	float4		upper;
} float4KEY;

/*
** float4 ops
*/
PG_FUNCTION_INFO_V1(gbt_float4_compress);
PG_FUNCTION_INFO_V1(gbt_float4_union);
PG_FUNCTION_INFO_V1(gbt_float4_picksplit);
PG_FUNCTION_INFO_V1(gbt_float4_consistent);
PG_FUNCTION_INFO_V1(gbt_float4_penalty);
PG_FUNCTION_INFO_V1(gbt_float4_same);

Datum		gbt_float4_compress(PG_FUNCTION_ARGS);
Datum		gbt_float4_union(PG_FUNCTION_ARGS);
Datum		gbt_float4_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_float4_consistent(PG_FUNCTION_ARGS);
Datum		gbt_float4_penalty(PG_FUNCTION_ARGS);
Datum		gbt_float4_same(PG_FUNCTION_ARGS);

static bool
gbt_float4gt(const void *a, const void *b)
{
	return (*((float4 *) a) > *((float4 *) b));
}
static bool
gbt_float4ge(const void *a, const void *b)
{
	return (*((float4 *) a) >= *((float4 *) b));
}
static bool
gbt_float4eq(const void *a, const void *b)
{
	return (*((float4 *) a) == *((float4 *) b));
}
static bool
gbt_float4le(const void *a, const void *b)
{
	return (*((float4 *) a) <= *((float4 *) b));
}
static bool
gbt_float4lt(const void *a, const void *b)
{
	return (*((float4 *) a) < *((float4 *) b));
}

static int
gbt_float4key_cmp(const void *a, const void *b)
{
    float4KEY    *ia = (float4KEY*)(((Nsrt *) a)->t);
	float4KEY    *ib = (float4KEY*)(((Nsrt *) b)->t);

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
	gbt_t_float4,
	sizeof(float4),
	gbt_float4gt,
	gbt_float4ge,
	gbt_float4eq,
	gbt_float4le,
	gbt_float4lt,
	gbt_float4key_cmp
};


/**************************************************
 * float4 ops
 **************************************************/


Datum
gbt_float4_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = NULL;

	PG_RETURN_POINTER(gbt_num_compress(retval, entry, &tinfo));
}


Datum
gbt_float4_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	float4		query = PG_GETARG_FLOAT4(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	float4KEY  *kkk = (float4KEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_BOOL(
				   gbt_num_consistent(&key, (void *) &query, &strategy, GIST_LEAF(entry), &tinfo)
		);
}


Datum
gbt_float4_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(float4KEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(float4KEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
}


Datum
gbt_float4_penalty(PG_FUNCTION_ARGS)
{
	float4KEY  *origentry = (float4KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	float4KEY  *newentry = (float4KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);

}

Datum
gbt_float4_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_float4_same(PG_FUNCTION_ARGS)
{
	float4KEY  *b1 = (float4KEY *) PG_GETARG_POINTER(0);
	float4KEY  *b2 = (float4KEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
