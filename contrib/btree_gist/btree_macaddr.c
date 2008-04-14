#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/builtins.h"
#include "utils/inet.h"

typedef struct
{
	macaddr		lower;
	macaddr		upper;
}	macKEY;

/*
** OID ops
*/
PG_FUNCTION_INFO_V1(gbt_macad_compress);
PG_FUNCTION_INFO_V1(gbt_macad_union);
PG_FUNCTION_INFO_V1(gbt_macad_picksplit);
PG_FUNCTION_INFO_V1(gbt_macad_consistent);
PG_FUNCTION_INFO_V1(gbt_macad_penalty);
PG_FUNCTION_INFO_V1(gbt_macad_same);

Datum		gbt_macad_compress(PG_FUNCTION_ARGS);
Datum		gbt_macad_union(PG_FUNCTION_ARGS);
Datum		gbt_macad_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_macad_consistent(PG_FUNCTION_ARGS);
Datum		gbt_macad_penalty(PG_FUNCTION_ARGS);
Datum		gbt_macad_same(PG_FUNCTION_ARGS);


static bool
gbt_macadgt(const void *a, const void *b)
{
	return DatumGetBool(DirectFunctionCall2(macaddr_gt, PointerGetDatum(a), PointerGetDatum(b)));
}
static bool
gbt_macadge(const void *a, const void *b)
{
	return DatumGetBool(DirectFunctionCall2(macaddr_ge, PointerGetDatum(a), PointerGetDatum(b)));
}

static bool
gbt_macadeq(const void *a, const void *b)
{
	return DatumGetBool(DirectFunctionCall2(macaddr_eq, PointerGetDatum(a), PointerGetDatum(b)));
}

static bool
gbt_macadle(const void *a, const void *b)
{
	return DatumGetBool(DirectFunctionCall2(macaddr_le, PointerGetDatum(a), PointerGetDatum(b)));
}

static bool
gbt_macadlt(const void *a, const void *b)
{
	return DatumGetBool(DirectFunctionCall2(macaddr_lt, PointerGetDatum(a), PointerGetDatum(b)));
}


static int
gbt_macadkey_cmp(const void *a, const void *b)
{
	return DatumGetInt32(
						 DirectFunctionCall2(
											 macaddr_cmp,
										PointerGetDatum(&((Nsrt *) a)->t[0]),
										 PointerGetDatum(&((Nsrt *) b)->t[0])
											 )
		);
}


static const gbtree_ninfo tinfo =
{
	gbt_t_macad,
	sizeof(macaddr),
	gbt_macadgt,
	gbt_macadge,
	gbt_macadeq,
	gbt_macadle,
	gbt_macadlt,
	gbt_macadkey_cmp
};


/**************************************************
 * macaddr ops
 **************************************************/



static uint64
mac_2_uint64(macaddr *m)
{
	unsigned char *mi = (unsigned char *) m;
	uint64		res = 0;
	int			i;

	for (i = 0; i < 6; i++)
		res += (((uint64) mi[i]) << ((uint64) ((5 - i) * 8)));
	return res;
}



Datum
gbt_macad_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = NULL;

	PG_RETURN_POINTER(gbt_num_compress(retval, entry, &tinfo));
}


Datum
gbt_macad_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	macaddr    *query = (macaddr *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	macKEY	   *kkk = (macKEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) & kkk->lower;
	key.upper = (GBT_NUMKEY *) & kkk->upper;

	PG_RETURN_BOOL(
				   gbt_num_consistent(&key, (void *) query, &strategy, GIST_LEAF(entry), &tinfo)
		);
}


Datum
gbt_macad_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(macKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(macKEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
}


Datum
gbt_macad_penalty(PG_FUNCTION_ARGS)
{
	macKEY	   *origentry = (macKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	macKEY	   *newentry = (macKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	uint64		iorg[2],
				inew[2];

	iorg[0] = mac_2_uint64(&origentry->lower);
	iorg[1] = mac_2_uint64(&origentry->upper);
	inew[0] = mac_2_uint64(&newentry->lower);
	inew[1] = mac_2_uint64(&newentry->upper);

	penalty_num(result, iorg[0], iorg[1], inew[0], inew[1]);

	PG_RETURN_POINTER(result);

}

Datum
gbt_macad_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_macad_same(PG_FUNCTION_ARGS)
{
	macKEY	   *b1 = (macKEY *) PG_GETARG_POINTER(0);
	macKEY	   *b2 = (macKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
