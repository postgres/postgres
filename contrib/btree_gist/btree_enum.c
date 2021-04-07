/*
 * contrib/btree_gist/btree_enum.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "fmgr.h"
#include "utils/builtins.h"

/* enums are really Oids, so we just use the same structure */

typedef struct
{
	Oid			lower;
	Oid			upper;
} oidKEY;

/*
** enum ops
*/
PG_FUNCTION_INFO_V1(gbt_enum_compress);
PG_FUNCTION_INFO_V1(gbt_enum_fetch);
PG_FUNCTION_INFO_V1(gbt_enum_union);
PG_FUNCTION_INFO_V1(gbt_enum_picksplit);
PG_FUNCTION_INFO_V1(gbt_enum_consistent);
PG_FUNCTION_INFO_V1(gbt_enum_penalty);
PG_FUNCTION_INFO_V1(gbt_enum_same);
PG_FUNCTION_INFO_V1(gbt_enum_sortsupport);


static bool
gbt_enumgt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(CallerFInfoFunctionCall2(enum_gt, flinfo, InvalidOid,
												 ObjectIdGetDatum(*((const Oid *) a)),
												 ObjectIdGetDatum(*((const Oid *) b))));
}
static bool
gbt_enumge(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(CallerFInfoFunctionCall2(enum_ge, flinfo, InvalidOid,
												 ObjectIdGetDatum(*((const Oid *) a)),
												 ObjectIdGetDatum(*((const Oid *) b))));
}
static bool
gbt_enumeq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const Oid *) a) == *((const Oid *) b));
}
static bool
gbt_enumle(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(CallerFInfoFunctionCall2(enum_le, flinfo, InvalidOid,
												 ObjectIdGetDatum(*((const Oid *) a)),
												 ObjectIdGetDatum(*((const Oid *) b))));
}
static bool
gbt_enumlt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(CallerFInfoFunctionCall2(enum_lt, flinfo, InvalidOid,
												 ObjectIdGetDatum(*((const Oid *) a)),
												 ObjectIdGetDatum(*((const Oid *) b))));
}

static int
gbt_enumkey_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	oidKEY	   *ia = (oidKEY *) (((const Nsrt *) a)->t);
	oidKEY	   *ib = (oidKEY *) (((const Nsrt *) b)->t);

	if (ia->lower == ib->lower)
	{
		if (ia->upper == ib->upper)
			return 0;

		return DatumGetInt32(CallerFInfoFunctionCall2(enum_cmp, flinfo, InvalidOid,
													  ObjectIdGetDatum(ia->upper),
													  ObjectIdGetDatum(ib->upper)));
	}

	return DatumGetInt32(CallerFInfoFunctionCall2(enum_cmp, flinfo, InvalidOid,
												  ObjectIdGetDatum(ia->lower),
												  ObjectIdGetDatum(ib->lower)));
}

static const gbtree_ninfo tinfo =
{
	gbt_t_enum,
	sizeof(Oid),
	8,							/* sizeof(gbtreekey8) */
	gbt_enumgt,
	gbt_enumge,
	gbt_enumeq,
	gbt_enumle,
	gbt_enumlt,
	gbt_enumkey_cmp,
	NULL						/* no KNN support at least for now */
};


/**************************************************
 * Enum ops
 **************************************************/


Datum
gbt_enum_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_compress(entry, &tinfo));
}

Datum
gbt_enum_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_fetch(entry, &tinfo));
}

Datum
gbt_enum_consistent(PG_FUNCTION_ARGS)
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

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_BOOL(gbt_num_consistent(&key, (void *) &query, &strategy,
									  GIST_LEAF(entry), &tinfo,
									  fcinfo->flinfo));
}

Datum
gbt_enum_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(oidKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(oidKEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo, fcinfo->flinfo));
}


Datum
gbt_enum_penalty(PG_FUNCTION_ARGS)
{
	oidKEY	   *origentry = (oidKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	oidKEY	   *newentry = (oidKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);
}

Datum
gbt_enum_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit((GistEntryVector *) PG_GETARG_POINTER(0),
										(GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo, fcinfo->flinfo));
}

Datum
gbt_enum_same(PG_FUNCTION_ARGS)
{
	oidKEY	   *b1 = (oidKEY *) PG_GETARG_POINTER(0);
	oidKEY	   *b2 = (oidKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}

static int
gbt_enum_sort_build_cmp(Datum a, Datum b, SortSupport ssup)
{
	oidKEY	   *ia = (oidKEY *) DatumGetPointer(a);
	oidKEY	   *ib = (oidKEY *) DatumGetPointer(b);

	/* for leaf items we expect lower == upper */
	Assert(ia->lower == ia->upper);
	Assert(ib->lower == ib->upper);

	if (ia->lower == ib->lower)
		return 0;

	return (ia->lower > ib->lower) ? 1 : -1;
}

static Datum
gbt_enum_abbrev_convert(Datum original, SortSupport ssup)
{
	oidKEY	   *b1 = (oidKEY *) DatumGetPointer(original);

	return ObjectIdGetDatum(b1->lower);
}

static int
gbt_enum_cmp_abbrev(Datum z1, Datum z2, SortSupport ssup)
{
	Oid			a = DatumGetObjectId(z1);
	Oid			b = DatumGetObjectId(z2);

	if (a > b)
		return 1;
	else if (a < b)
		return -1;
	else
		return 0;
}

/*
 * We never consider aborting the abbreviation.
 */
static bool
gbt_enum_abbrev_abort(int memtupcount, SortSupport ssup)
{
	return false;
}

/*
 * Sort support routine for fast GiST index build by sorting.
 */
Datum
gbt_enum_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	if (ssup->abbreviate)
	{
		ssup->comparator = gbt_enum_cmp_abbrev;
		ssup->abbrev_converter = gbt_enum_abbrev_convert;
		ssup->abbrev_abort = gbt_enum_abbrev_abort;
		ssup->abbrev_full_comparator = gbt_enum_sort_build_cmp;
	}
	else
	{
		ssup->comparator = gbt_enum_sort_build_cmp;
	}
	PG_RETURN_VOID();
}
