/*
 * contrib/btree_gist/btree_date.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/fmgrprotos.h"
#include "utils/date.h"
#include "utils/rel.h"
#include "utils/sortsupport.h"

typedef struct
{
	DateADT		lower;
	DateADT		upper;
} dateKEY;

/* GiST support functions */
PG_FUNCTION_INFO_V1(gbt_date_compress);
PG_FUNCTION_INFO_V1(gbt_date_fetch);
PG_FUNCTION_INFO_V1(gbt_date_union);
PG_FUNCTION_INFO_V1(gbt_date_picksplit);
PG_FUNCTION_INFO_V1(gbt_date_consistent);
PG_FUNCTION_INFO_V1(gbt_date_distance);
PG_FUNCTION_INFO_V1(gbt_date_penalty);
PG_FUNCTION_INFO_V1(gbt_date_same);
PG_FUNCTION_INFO_V1(gbt_date_sortsupport);

static bool
gbt_dategt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(date_gt,
											DateADTGetDatum(*((const DateADT *) a)),
											DateADTGetDatum(*((const DateADT *) b))));
}

static bool
gbt_datege(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(date_ge,
											DateADTGetDatum(*((const DateADT *) a)),
											DateADTGetDatum(*((const DateADT *) b))));
}

static bool
gbt_dateeq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(date_eq,
											DateADTGetDatum(*((const DateADT *) a)),
											DateADTGetDatum(*((const DateADT *) b)))
		);
}

static bool
gbt_datele(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(date_le,
											DateADTGetDatum(*((const DateADT *) a)),
											DateADTGetDatum(*((const DateADT *) b))));
}

static bool
gbt_datelt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(date_lt,
											DateADTGetDatum(*((const DateADT *) a)),
											DateADTGetDatum(*((const DateADT *) b))));
}



static int
gbt_datekey_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	dateKEY    *ia = (dateKEY *) (((const Nsrt *) a)->t);
	dateKEY    *ib = (dateKEY *) (((const Nsrt *) b)->t);
	int			res;

	res = DatumGetInt32(DirectFunctionCall2(date_cmp,
											DateADTGetDatum(ia->lower),
											DateADTGetDatum(ib->lower)));
	if (res == 0)
		return DatumGetInt32(DirectFunctionCall2(date_cmp,
												 DateADTGetDatum(ia->upper),
												 DateADTGetDatum(ib->upper)));

	return res;
}

static float8
gdb_date_dist(const void *a, const void *b, FmgrInfo *flinfo)
{
	DateADT		da = *((const DateADT *) a);
	DateADT		db = *((const DateADT *) b);

	/*
	 * Treat same-sign infinities as equal, as the float8 opclass does; but
	 * any other case involving infinite inputs yields infinite distance.
	 */
	if (DATE_NOT_FINITE(da) || DATE_NOT_FINITE(db))
		return (da == db) ? 0.0 : get_float8_infinity();

	/* the difference of two finite dates cannot overflow int32 */
	return (float8) abs(da - db);
}


static const gbtree_ninfo tinfo =
{
	gbt_t_date,
	sizeof(DateADT),
	8,							/* sizeof(gbtreekey8) */
	gbt_dategt,
	gbt_datege,
	gbt_dateeq,
	gbt_datele,
	gbt_datelt,
	gbt_datekey_cmp,
	gdb_date_dist
};


PG_FUNCTION_INFO_V1(date_dist);
Datum
date_dist(PG_FUNCTION_ARGS)
{
	DateADT		a = PG_GETARG_DATEADT(0);
	DateADT		b = PG_GETARG_DATEADT(1);

	/*
	 * As in gdb_date_dist, same-sign infinities are equal.  For other cases
	 * involving infinite inputs, int4 has no infinity, so use PG_INT32_MAX.
	 * This is consistent with the ordering produced by gdb_date_dist, since
	 * no difference of two finite dates reaches PG_INT32_MAX.
	 */
	if (DATE_NOT_FINITE(a) || DATE_NOT_FINITE(b))
		PG_RETURN_INT32((a == b) ? 0 : PG_INT32_MAX);

	/* the difference of two finite dates cannot overflow int32 */
	PG_RETURN_INT32(abs(a - b));
}


/**************************************************
 * GiST support functions
 **************************************************/

Datum
gbt_date_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_compress(entry, &tinfo));
}

Datum
gbt_date_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_fetch(entry, &tinfo));
}

Datum
gbt_date_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	DateADT		query = PG_GETARG_DATEADT(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
#ifdef NOT_USED
	Oid			subtype = PG_GETARG_OID(3);
#endif
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	dateKEY    *kkk = (dateKEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_BOOL(gbt_num_consistent(&key, &query, strategy,
									  GIST_LEAF(entry), &tinfo,
									  fcinfo->flinfo));
}

Datum
gbt_date_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	DateADT		query = PG_GETARG_DATEADT(1);
#ifdef NOT_USED
	Oid			subtype = PG_GETARG_OID(3);
#endif
	dateKEY    *kkk = (dateKEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_FLOAT8(gbt_num_distance(&key, &query, GIST_LEAF(entry),
									  &tinfo, fcinfo->flinfo));
}

Datum
gbt_date_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc_object(dateKEY);

	*(int *) PG_GETARG_POINTER(1) = sizeof(dateKEY);
	PG_RETURN_POINTER(gbt_num_union(out, entryvec, &tinfo, fcinfo->flinfo));
}

Datum
gbt_date_penalty(PG_FUNCTION_ARGS)
{
	dateKEY    *origentry = (dateKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	dateKEY    *newentry = (dateKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	/*
	 * Don't use date_mi here: it rejects infinite dates.  Plain int32
	 * subtraction won't do either: it could overflow for cases involving
	 * infinite dates.  But we can use penalty_num: it computes in double,
	 * where every int32 and every difference of two int32s is exact.  The
	 * output for cases with unequal infinite dates will be finite but large,
	 * which seems fine for penalty calculations.
	 */
	penalty_num(result, origentry->lower, origentry->upper,
				newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);
}

Datum
gbt_date_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit((GistEntryVector *) PG_GETARG_POINTER(0),
										(GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo, fcinfo->flinfo));
}

Datum
gbt_date_same(PG_FUNCTION_ARGS)
{
	dateKEY    *b1 = (dateKEY *) PG_GETARG_POINTER(0);
	dateKEY    *b2 = (dateKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}

static int
gbt_date_ssup_cmp(Datum x, Datum y, SortSupport ssup)
{
	dateKEY    *akey = (dateKEY *) DatumGetPointer(x);
	dateKEY    *bkey = (dateKEY *) DatumGetPointer(y);

	/* for leaf items we expect lower == upper, so only compare lower */
	return DatumGetInt32(DirectFunctionCall2(date_cmp,
											 DateADTGetDatum(akey->lower),
											 DateADTGetDatum(bkey->lower)));
}

Datum
gbt_date_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = gbt_date_ssup_cmp;
	ssup->ssup_extra = NULL;

	PG_RETURN_VOID();
}
