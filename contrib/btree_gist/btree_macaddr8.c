/*
 * contrib/btree_gist/btree_macaddr8.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/builtins.h"
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
PG_FUNCTION_INFO_V1(gbt_macad8_sortsupport);


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

static int
gbt_macad8_sort_build_cmp(Datum a, Datum b, SortSupport ssup)
{
	mac8KEY    *ma = (mac8KEY *) DatumGetPointer(a);
	mac8KEY    *mb = (mac8KEY *) DatumGetPointer(b);
	uint64		ia = mac8_2_uint64(&ma->lower);
	uint64		ib = mac8_2_uint64(&mb->lower);

	/* for leaf items we expect lower == upper */

	if (ia == ib)
		return 0;

	return (ia > ib) ? 1 : -1;
}

static Datum
gbt_macad8_abbrev_convert(Datum original, SortSupport ssup)
{
	mac8KEY    *b1 = (mac8KEY *) DatumGetPointer(original);
	uint64		z = mac8_2_uint64(&b1->lower);

#if SIZEOF_DATUM == 8
	return UInt64GetDatum(z);
#else
	/* use the high bits only */
	return UInt32GetDatum(z >> 32);
#endif
}

static int
gbt_macad8_cmp_abbrev(Datum z1, Datum z2, SortSupport ssup)
{
#if SIZEOF_DATUM == 8
	uint64		a = DatumGetUInt64(z1);
	uint64		b = DatumGetUInt64(z2);
#else
	uint32		a = DatumGetUInt32(z1);
	uint32		b = DatumGetUInt32(z2);
#endif

	if (a > b)
		return 1;
	else if (a < b)
		return -1;
	else
		return 0;
}

static bool
gbt_macad8_abbrev_abort(int memtupcount, SortSupport ssup)
{
	return false;
}

/*
 * Sort support routine for fast GiST index build by sorting.
 */
Datum
gbt_macad8_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	if (ssup->abbreviate)
	{
		ssup->comparator = gbt_macad8_cmp_abbrev;
		ssup->abbrev_converter = gbt_macad8_abbrev_convert;
		ssup->abbrev_abort = gbt_macad8_abbrev_abort;
		ssup->abbrev_full_comparator = gbt_macad8_sort_build_cmp;
	}
	else
	{
		ssup->comparator = gbt_macad8_sort_build_cmp;
	}
	PG_RETURN_VOID();
}
