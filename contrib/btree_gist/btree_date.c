/*
 * contrib/btree_gist/btree_date.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/builtins.h"
#include "utils/date.h"

typedef struct
{
	DateADT		lower;
	DateADT		upper;
} dateKEY;

/*
** date ops
*/
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
	/* we assume the difference can't overflow */
	Datum		diff = DirectFunctionCall2(date_mi,
										   DateADTGetDatum(*((const DateADT *) a)),
										   DateADTGetDatum(*((const DateADT *) b)));

	return (float8) Abs(DatumGetInt32(diff));
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
	/* we assume the difference can't overflow */
	Datum		diff = DirectFunctionCall2(date_mi,
										   PG_GETARG_DATUM(0),
										   PG_GETARG_DATUM(1));

	PG_RETURN_INT32(Abs(DatumGetInt32(diff)));
}


/**************************************************
 * date ops
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

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	dateKEY    *kkk = (dateKEY *) DatumGetPointer(entry->key);
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
gbt_date_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	DateADT		query = PG_GETARG_DATEADT(1);

	/* Oid		subtype = PG_GETARG_OID(3); */
	dateKEY    *kkk = (dateKEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_FLOAT8(gbt_num_distance(&key, (void *) &query, GIST_LEAF(entry),
									  &tinfo, fcinfo->flinfo));
}


Datum
gbt_date_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(dateKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(dateKEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo, fcinfo->flinfo));
}


Datum
gbt_date_penalty(PG_FUNCTION_ARGS)
{
	dateKEY    *origentry = (dateKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	dateKEY    *newentry = (dateKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	int32		diff,
				res;

	diff = DatumGetInt32(DirectFunctionCall2(date_mi,
											 DateADTGetDatum(newentry->upper),
											 DateADTGetDatum(origentry->upper)));

	res = Max(diff, 0);

	diff = DatumGetInt32(DirectFunctionCall2(date_mi,
											 DateADTGetDatum(origentry->lower),
											 DateADTGetDatum(newentry->lower)));

	res += Max(diff, 0);

	*result = 0.0;

	if (res > 0)
	{
		diff = DatumGetInt32(DirectFunctionCall2(date_mi,
												 DateADTGetDatum(origentry->upper),
												 DateADTGetDatum(origentry->lower)));
		*result += FLT_MIN;
		*result += (float) (res / ((double) (res + diff)));
		*result *= (FLT_MAX / (((GISTENTRY *) PG_GETARG_POINTER(0))->rel->rd_att->natts + 1));
	}

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
gbt_date_sort_build_cmp(Datum a, Datum b, SortSupport ssup)
{
	dateKEY    *ia = (dateKEY *) PointerGetDatum(a);
	dateKEY    *ib = (dateKEY *) PointerGetDatum(b);

	return DatumGetInt32(DirectFunctionCall2(date_cmp,
											 DateADTGetDatum(ia->lower),
											 DateADTGetDatum(ib->lower)));
}

/*
 * Sort support routine for fast GiST index build by sorting.
 */
Datum
gbt_date_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = gbt_date_sort_build_cmp;
	ssup->abbrev_converter = NULL;
	ssup->abbrev_abort = NULL;
	ssup->abbrev_full_comparator = NULL;
	PG_RETURN_VOID();
}
