#include "btree_gist.h"
#include "btree_utils_num.h"

typedef struct
{
	Oid			lower;
	Oid			upper;
}	oidKEY;

/*
** OID ops
*/
PG_FUNCTION_INFO_V1(gbt_oid_compress);
PG_FUNCTION_INFO_V1(gbt_oid_union);
PG_FUNCTION_INFO_V1(gbt_oid_picksplit);
PG_FUNCTION_INFO_V1(gbt_oid_consistent);
PG_FUNCTION_INFO_V1(gbt_oid_penalty);
PG_FUNCTION_INFO_V1(gbt_oid_same);

Datum		gbt_oid_compress(PG_FUNCTION_ARGS);
Datum		gbt_oid_union(PG_FUNCTION_ARGS);
Datum		gbt_oid_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_oid_consistent(PG_FUNCTION_ARGS);
Datum		gbt_oid_penalty(PG_FUNCTION_ARGS);
Datum		gbt_oid_same(PG_FUNCTION_ARGS);


static bool
gbt_oidgt(const void *a, const void *b)
{
	return (*((Oid *) a) > *((Oid *) b));
}
static bool
gbt_oidge(const void *a, const void *b)
{
	return (*((Oid *) a) >= *((Oid *) b));
}
static bool
gbt_oideq(const void *a, const void *b)
{
	return (*((Oid *) a) == *((Oid *) b));
}
static bool
gbt_oidle(const void *a, const void *b)
{
	return (*((Oid *) a) <= *((Oid *) b));
}
static bool
gbt_oidlt(const void *a, const void *b)
{
	return (*((Oid *) a) < *((Oid *) b));
}

static int
gbt_oidkey_cmp(const void *a, const void *b)
{

	if (*(Oid *) &(((Nsrt *) a)->t[0]) > *(Oid *) &(((Nsrt *) b)->t[0]))
		return 1;
	else if (*(Oid *) &(((Nsrt *) a)->t[0]) < *(Oid *) &(((Nsrt *) b)->t[0]))
		return -1;
	return 0;

}


static const gbtree_ninfo tinfo =
{
	gbt_t_oid,
	sizeof(Oid),
	gbt_oidgt,
	gbt_oidge,
	gbt_oideq,
	gbt_oidle,
	gbt_oidlt,
	gbt_oidkey_cmp
};


/**************************************************
 * Oid ops
 **************************************************/


Datum
gbt_oid_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = NULL;

	PG_RETURN_POINTER(gbt_num_compress(retval, entry, &tinfo));
}


Datum
gbt_oid_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Oid			query = PG_GETARG_OID(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	oidKEY	   *kkk = (oidKEY *) DatumGetPointer(entry->key);
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
gbt_oid_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(oidKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(oidKEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
}


Datum
gbt_oid_penalty(PG_FUNCTION_ARGS)
{
	oidKEY	   *origentry = (oidKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	oidKEY	   *newentry = (oidKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);
}

Datum
gbt_oid_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_oid_same(PG_FUNCTION_ARGS)
{
	oidKEY	   *b1 = (oidKEY *) PG_GETARG_POINTER(0);
	oidKEY	   *b2 = (oidKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
