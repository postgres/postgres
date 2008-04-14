#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/date.h"

typedef struct
{
	DateADT		lower;
	DateADT		upper;
}	dateKEY;

/*
** date ops
*/
PG_FUNCTION_INFO_V1(gbt_date_compress);
PG_FUNCTION_INFO_V1(gbt_date_union);
PG_FUNCTION_INFO_V1(gbt_date_picksplit);
PG_FUNCTION_INFO_V1(gbt_date_consistent);
PG_FUNCTION_INFO_V1(gbt_date_penalty);
PG_FUNCTION_INFO_V1(gbt_date_same);

Datum		gbt_date_compress(PG_FUNCTION_ARGS);
Datum		gbt_date_union(PG_FUNCTION_ARGS);
Datum		gbt_date_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_date_consistent(PG_FUNCTION_ARGS);
Datum		gbt_date_penalty(PG_FUNCTION_ARGS);
Datum		gbt_date_same(PG_FUNCTION_ARGS);

static bool
gbt_dategt(const void *a, const void *b)
{
	return DatumGetBool(
						DirectFunctionCall2(date_gt, DateADTGetDatum(*((DateADT *) a)), DateADTGetDatum(*((DateADT *) b)))
		);
}

static bool
gbt_datege(const void *a, const void *b)
{
	return DatumGetBool(
						DirectFunctionCall2(date_ge, DateADTGetDatum(*((DateADT *) a)), DateADTGetDatum(*((DateADT *) b)))
		);
}

static bool
gbt_dateeq(const void *a, const void *b)
{
	return DatumGetBool(
						DirectFunctionCall2(date_eq, DateADTGetDatum(*((DateADT *) a)), DateADTGetDatum(*((DateADT *) b)))
		);
}

static bool
gbt_datele(const void *a, const void *b)
{
	return DatumGetBool(
						DirectFunctionCall2(date_le, DateADTGetDatum(*((DateADT *) a)), DateADTGetDatum(*((DateADT *) b)))
		);
}

static bool
gbt_datelt(const void *a, const void *b)
{
	return DatumGetBool(
						DirectFunctionCall2(date_lt, DateADTGetDatum(*((DateADT *) a)), DateADTGetDatum(*((DateADT *) b)))
		);
}



static int
gbt_datekey_cmp(const void *a, const void *b)
{
	if (gbt_dategt((void *) &(((Nsrt *) a)->t[0]), (void *) &(((Nsrt *) b)->t[0])))
		return 1;
	else if (gbt_datelt((void *) &(((Nsrt *) a)->t[0]), (void *) &(((Nsrt *) b)->t[0])))
		return -1;
	return 0;
}


static const gbtree_ninfo tinfo =
{
	gbt_t_date,
	sizeof(DateADT),
	gbt_dategt,
	gbt_datege,
	gbt_dateeq,
	gbt_datele,
	gbt_datelt,
	gbt_datekey_cmp
};


/**************************************************
 * date ops
 **************************************************/



Datum
gbt_date_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = NULL;

	PG_RETURN_POINTER(gbt_num_compress(retval, entry, &tinfo));
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

	key.lower = (GBT_NUMKEY *) & kkk->lower;
	key.upper = (GBT_NUMKEY *) & kkk->upper;

	PG_RETURN_BOOL(
				   gbt_num_consistent(&key, (void *) &query, &strategy, GIST_LEAF(entry), &tinfo)
		);
}


Datum
gbt_date_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(dateKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(dateKEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
}


Datum
gbt_date_penalty(PG_FUNCTION_ARGS)
{
	dateKEY    *origentry = (dateKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	dateKEY    *newentry = (dateKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	int32		diff,
				res;

	diff = DatumGetInt32(DirectFunctionCall2(
											 date_mi,
											 DateADTGetDatum(newentry->upper),
										 DateADTGetDatum(origentry->upper)));

	res = Max(diff, 0);

	diff = DatumGetInt32(DirectFunctionCall2(
											 date_mi,
										   DateADTGetDatum(origentry->lower),
										  DateADTGetDatum(newentry->lower)));

	res += Max(diff, 0);

	*result = 0.0;

	if (res > 0)
	{
		diff = DatumGetInt32(DirectFunctionCall2(
												 date_mi,
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
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_date_same(PG_FUNCTION_ARGS)
{
	dateKEY    *b1 = (dateKEY *) PG_GETARG_POINTER(0);
	dateKEY    *b2 = (dateKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
