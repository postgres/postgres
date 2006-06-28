#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/builtins.h"
#include "utils/inet.h"
#include "catalog/pg_type.h"

typedef struct inetkey
{
	double		lower;
	double		upper;
}	inetKEY;

/*
** inet ops
*/
PG_FUNCTION_INFO_V1(gbt_inet_compress);
PG_FUNCTION_INFO_V1(gbt_inet_union);
PG_FUNCTION_INFO_V1(gbt_inet_picksplit);
PG_FUNCTION_INFO_V1(gbt_inet_consistent);
PG_FUNCTION_INFO_V1(gbt_inet_penalty);
PG_FUNCTION_INFO_V1(gbt_inet_same);

Datum		gbt_inet_compress(PG_FUNCTION_ARGS);
Datum		gbt_inet_union(PG_FUNCTION_ARGS);
Datum		gbt_inet_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_inet_consistent(PG_FUNCTION_ARGS);
Datum		gbt_inet_penalty(PG_FUNCTION_ARGS);
Datum		gbt_inet_same(PG_FUNCTION_ARGS);


static bool
gbt_inetgt(const void *a, const void *b)
{
	return (*((double *) a) > *((double *) b));
}
static bool
gbt_inetge(const void *a, const void *b)
{
	return (*((double *) a) >= *((double *) b));
}
static bool
gbt_ineteq(const void *a, const void *b)
{
	return (*((double *) a) == *((double *) b));
}
static bool
gbt_inetle(const void *a, const void *b)
{
	return (*((double *) a) <= *((double *) b));
}
static bool
gbt_inetlt(const void *a, const void *b)
{
	return (*((double *) a) < *((double *) b));
}

static int
gbt_inetkey_cmp(const void *a, const void *b)
{

	if (*(double *) (&((Nsrt *) a)->t[0]) > *(double *) (&((Nsrt *) b)->t[0]))
		return 1;
	else if (*(double *) (&((Nsrt *) a)->t[0]) < *(double *) (&((Nsrt *) b)->t[0]))
		return -1;
	return 0;

}


static const gbtree_ninfo tinfo =
{
	gbt_t_inet,
	sizeof(double),
	gbt_inetgt,
	gbt_inetge,
	gbt_ineteq,
	gbt_inetle,
	gbt_inetlt,
	gbt_inetkey_cmp
};


/**************************************************
 * inet ops
 **************************************************/


Datum
gbt_inet_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		inetKEY    *r = (inetKEY *) palloc(sizeof(inetKEY));

		retval = palloc(sizeof(GISTENTRY));
		r->lower = convert_network_to_scalar(entry->key, INETOID);
		r->upper = r->lower;
		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
	}
	else
		retval = entry;

	PG_RETURN_POINTER(retval);
}


Datum
gbt_inet_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	double		query = convert_network_to_scalar(PG_GETARG_DATUM(1), INETOID);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	inetKEY    *kkk = (inetKEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	key.lower = (GBT_NUMKEY *) & kkk->lower;
	key.upper = (GBT_NUMKEY *) & kkk->upper;

	PG_RETURN_BOOL(gbt_num_consistent(&key, (void *) &query,
									  &strategy, GIST_LEAF(entry), &tinfo));
}


Datum
gbt_inet_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(inetKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(inetKEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
}


Datum
gbt_inet_penalty(PG_FUNCTION_ARGS)
{
	inetKEY    *origentry = (inetKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	inetKEY    *newentry = (inetKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);

}

Datum
gbt_inet_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_inet_same(PG_FUNCTION_ARGS)
{
	inetKEY    *b1 = (inetKEY *) PG_GETARG_POINTER(0);
	inetKEY    *b2 = (inetKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
