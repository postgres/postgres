/*
 * contrib/btree_gist/btree_macaddr8.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/fmgrprotos.h"
#include "utils/inet.h"

typedef struct
{
	macaddr8	lower;
	macaddr8	upper;
	/* make struct size = sizeof(gbtreekey16) */
} mac8KEY;

/*
** OID ops
*/
PG_FUNCTION_INFO_V1(gbt_macad8_compress);
PG_FUNCTION_INFO_V1(gbt_macad8_fetch);
PG_FUNCTION_INFO_V1(gbt_macad8_union);
PG_FUNCTION_INFO_V1(gbt_macad8_picksplit);
PG_FUNCTION_INFO_V1(gbt_macad8_consistent);
PG_FUNCTION_INFO_V1(gbt_macad8_penalty);
PG_FUNCTION_INFO_V1(gbt_macad8_same);


static bool
gbt_macad8gt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(macaddr8_gt, PointerGetDatum(a), PointerGetDatum(b)));
}
static bool
gbt_macad8ge(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(macaddr8_ge, PointerGetDatum(a), PointerGetDatum(b)));
}

static bool
gbt_macad8eq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(macaddr8_eq, PointerGetDatum(a), PointerGetDatum(b)));
}

static bool
gbt_macad8le(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(macaddr8_le, PointerGetDatum(a), PointerGetDatum(b)));
}

static bool
gbt_macad8lt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(macaddr8_lt, PointerGetDatum(a), PointerGetDatum(b)));
}


static int
gbt_macad8key_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	mac8KEY    *ia = (mac8KEY *) (((const Nsrt *) a)->t);
	mac8KEY    *ib = (mac8KEY *) (((const Nsrt *) b)->t);
	int			res;

	res = DatumGetInt32(DirectFunctionCall2(macaddr8_cmp, Macaddr8PGetDatum(&ia->lower), Macaddr8PGetDatum(&ib->lower)));
	if (res == 0)
		return DatumGetInt32(DirectFunctionCall2(macaddr8_cmp, Macaddr8PGetDatum(&ia->upper), Macaddr8PGetDatum(&ib->upper)));

	return res;
}


static const gbtree_ninfo tinfo =
{
	gbt_t_macad8,
	sizeof(macaddr8),
	16,							/* sizeof(gbtreekey16) */
	gbt_macad8gt,
	gbt_macad8ge,
	gbt_macad8eq,
	gbt_macad8le,
	gbt_macad8lt,
	gbt_macad8key_cmp,
	NULL
};


/**************************************************
 * macaddr ops
 **************************************************/



static uint64
mac8_2_uint64(macaddr8 *m)
{
	unsigned char *mi = (unsigned char *) m;
	uint64		res = 0;
	int			i;

	for (i = 0; i < 8; i++)
		res += (((uint64) mi[i]) << ((uint64) ((7 - i) * 8)));
	return res;
}



Datum
gbt_macad8_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_compress(entry, &tinfo));
}

Datum
gbt_macad8_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_fetch(entry, &tinfo));
}

Datum
gbt_macad8_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	macaddr8   *query = (macaddr8 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	mac8KEY    *kkk = (mac8KEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_BOOL(gbt_num_consistent(&key, (void *) query, &strategy,
									  GIST_LEAF(entry), &tinfo, fcinfo->flinfo));
}


Datum
gbt_macad8_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc0(sizeof(mac8KEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(mac8KEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo, fcinfo->flinfo));
}


Datum
gbt_macad8_penalty(PG_FUNCTION_ARGS)
{
	mac8KEY    *origentry = (mac8KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	mac8KEY    *newentry = (mac8KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	uint64		iorg[2],
				inew[2];

	iorg[0] = mac8_2_uint64(&origentry->lower);
	iorg[1] = mac8_2_uint64(&origentry->upper);
	inew[0] = mac8_2_uint64(&newentry->lower);
	inew[1] = mac8_2_uint64(&newentry->upper);

	penalty_num(result, iorg[0], iorg[1], inew[0], inew[1]);

	PG_RETURN_POINTER(result);
}

Datum
gbt_macad8_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit((GistEntryVector *) PG_GETARG_POINTER(0),
										(GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo, fcinfo->flinfo));
}

Datum
gbt_macad8_same(PG_FUNCTION_ARGS)
{
	mac8KEY    *b1 = (mac8KEY *) PG_GETARG_POINTER(0);
	mac8KEY    *b2 = (mac8KEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}
