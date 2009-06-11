/*
 * $PostgreSQL: pgsql/contrib/btree_gist/btree_int2.c,v 1.8 2009/06/11 14:48:50 momjian Exp $
 */
#include "btree_gist.h"
#include "btree_utils_num.h"

typedef struct int16key
{
	int16		lower;
	int16		upper;
} int16KEY;

/*
** int16 ops
*/
PG_FUNCTION_INFO_V1(gbt_int2_compress);
PG_FUNCTION_INFO_V1(gbt_int2_union);
PG_FUNCTION_INFO_V1(gbt_int2_picksplit);
PG_FUNCTION_INFO_V1(gbt_int2_consistent);
PG_FUNCTION_INFO_V1(gbt_int2_penalty);
PG_FUNCTION_INFO_V1(gbt_int2_same);

Datum		gbt_int2_compress(PG_FUNCTION_ARGS);
Datum		gbt_int2_union(PG_FUNCTION_ARGS);
Datum		gbt_int2_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_int2_consistent(PG_FUNCTION_ARGS);
Datum		gbt_int2_penalty(PG_FUNCTION_ARGS);
Datum		gbt_int2_same(PG_FUNCTION_ARGS);

static bool
gbt_int2gt(const void *a, const void *b)
{
	return (*((int16 *) a) > *((int16 *) b));
}
static bool
gbt_int2ge(const void *a, const void *b)
{
	return (*((int16 *) a) >= *((int16 *) b));
}
static bool
gbt_int2eq(const void *a, const void *b)
{
	return (*((int16 *) a) == *((int16 *) b));
}
static bool
gbt_int2le(const void *a, const void *b)
{
	return (*((int16 *) a) <= *((int16 *) b));
}
static bool
gbt_int2lt(const void *a, const void *b)
{
	return (*((int16 *) a) < *((int16 *) b));
}

static int
gbt_int2key_cmp(const void *a, const void *b)
{

	if (*(int16 *) (&((Nsrt *) a)->t[0]) > *(int16 *) &(((Nsrt *) b)->t[0]))
		return 1;
	else if (*(int16 *) &(((Nsrt *) a)->t[0]) < *(int16 *) &(((Nsrt *) b)->t[0]))
		return -1;
	return 0;

}


static const gbtree_ninfo tinfo =
{
	gbt_t_int2,
	sizeof(int16),
	gbt_int2gt,
	gbt_int2ge,
	gbt_int2eq,
	gbt_int2le,
	gbt_int2lt,
	gbt_int2key_cmp
};






/**************************************************
 * int16 ops
 **************************************************/


Datum
gbt_int2_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = NULL;

	PG_RETURN_POINTER(gbt_num_compress(retval, entry, &tinfo));
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

	PG_RETURN_BOOL(
				   gbt_num_consistent(&key, (void *) &query, &strategy, GIST_LEAF(entry), &tinfo)
		);
}


Datum
gbt_int2_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(int16KEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(int16KEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
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
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_int2_same(PG_FUNCTION_ARGS)
{
	int16KEY   *b1 = (int16KEY *) PG_GETARG_POINTER(0);
	int16KEY   *b2 = (int16KEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
