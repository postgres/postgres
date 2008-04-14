#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/date.h"
#include "utils/timestamp.h"

typedef struct
{
	TimeADT		lower;
	TimeADT		upper;
}	timeKEY;

/*
** time ops
*/
PG_FUNCTION_INFO_V1(gbt_time_compress);
PG_FUNCTION_INFO_V1(gbt_timetz_compress);
PG_FUNCTION_INFO_V1(gbt_time_union);
PG_FUNCTION_INFO_V1(gbt_time_picksplit);
PG_FUNCTION_INFO_V1(gbt_time_consistent);
PG_FUNCTION_INFO_V1(gbt_timetz_consistent);
PG_FUNCTION_INFO_V1(gbt_time_penalty);
PG_FUNCTION_INFO_V1(gbt_time_same);

Datum		gbt_time_compress(PG_FUNCTION_ARGS);
Datum		gbt_timetz_compress(PG_FUNCTION_ARGS);
Datum		gbt_time_union(PG_FUNCTION_ARGS);
Datum		gbt_time_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_time_consistent(PG_FUNCTION_ARGS);
Datum		gbt_timetz_consistent(PG_FUNCTION_ARGS);
Datum		gbt_time_penalty(PG_FUNCTION_ARGS);
Datum		gbt_time_same(PG_FUNCTION_ARGS);


#define P_TimeADTGetDatum(x)	PointerGetDatum( &(x) )

static bool
gbt_timegt(const void *a, const void *b)
{
	return DatumGetBool(
		 DirectFunctionCall2(time_gt, PointerGetDatum(a), PointerGetDatum(b))
		);
}

static bool
gbt_timege(const void *a, const void *b)
{
	return DatumGetBool(
		 DirectFunctionCall2(time_ge, PointerGetDatum(a), PointerGetDatum(b))
		);
}

static bool
gbt_timeeq(const void *a, const void *b)
{
	return DatumGetBool(
		 DirectFunctionCall2(time_eq, PointerGetDatum(a), PointerGetDatum(b))
		);
}

static bool
gbt_timele(const void *a, const void *b)
{
	return DatumGetBool(
		 DirectFunctionCall2(time_le, PointerGetDatum(a), PointerGetDatum(b))
		);
}

static bool
gbt_timelt(const void *a, const void *b)
{
	return DatumGetBool(
		 DirectFunctionCall2(time_lt, PointerGetDatum(a), PointerGetDatum(b))
		);
}



static int
gbt_timekey_cmp(const void *a, const void *b)
{
	if (gbt_timegt((void *) &(((Nsrt *) a)->t[0]), (void *) &(((Nsrt *) b)->t[0])))
		return 1;
	else if (gbt_timelt((void *) &(((Nsrt *) a)->t[0]), (void *) &(((Nsrt *) b)->t[0])))
		return -1;
	return 0;
}


static const gbtree_ninfo tinfo =
{
	gbt_t_time,
	sizeof(TimeADT),
	gbt_timegt,
	gbt_timege,
	gbt_timeeq,
	gbt_timele,
	gbt_timelt,
	gbt_timekey_cmp
};


/**************************************************
 * time ops
 **************************************************/



Datum
gbt_time_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = NULL;

	PG_RETURN_POINTER(gbt_num_compress(retval, entry, &tinfo));
}


Datum
gbt_timetz_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		timeKEY    *r = (timeKEY *) palloc(sizeof(timeKEY));
		TimeTzADT  *tz = DatumGetTimeTzADTP(entry->key);
		TimeADT		tmp;

		retval = palloc(sizeof(GISTENTRY));

		/* We are using the time + zone only to compress */
#ifdef HAVE_INT64_TIMESTAMP
		tmp = tz->time + (tz->zone * INT64CONST(1000000));
#else
		tmp = (tz->time + tz->zone);
#endif
		r->lower = r->upper = tmp;
		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}


Datum
gbt_time_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	TimeADT		query = PG_GETARG_TIMEADT(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	timeKEY    *kkk = (timeKEY *) DatumGetPointer(entry->key);
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
gbt_timetz_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	TimeTzADT  *query = PG_GETARG_TIMETZADT_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	timeKEY    *kkk = (timeKEY *) DatumGetPointer(entry->key);
	TimeADT		qqq;
	GBT_NUMKEY_R key;

	/* All cases served by this function are inexact */
	*recheck = true;

#ifdef HAVE_INT64_TIMESTAMP
	qqq = query->time + (query->zone * INT64CONST(1000000));
#else
	qqq = (query->time + query->zone);
#endif

	key.lower = (GBT_NUMKEY *) & kkk->lower;
	key.upper = (GBT_NUMKEY *) & kkk->upper;

	PG_RETURN_BOOL(
				   gbt_num_consistent(&key, (void *) &qqq, &strategy, GIST_LEAF(entry), &tinfo)
		);
}


Datum
gbt_time_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(timeKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(timeKEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
}


Datum
gbt_time_penalty(PG_FUNCTION_ARGS)
{
	timeKEY    *origentry = (timeKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	timeKEY    *newentry = (timeKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	Interval   *intr;
	double		res;
	double		res2;

	intr = DatumGetIntervalP(DirectFunctionCall2(
												 time_mi_time,
										  P_TimeADTGetDatum(newentry->upper),
									   P_TimeADTGetDatum(origentry->upper)));
	res = INTERVAL_TO_SEC(intr);
	res = Max(res, 0);

	intr = DatumGetIntervalP(DirectFunctionCall2(
												 time_mi_time,
										 P_TimeADTGetDatum(origentry->lower),
										P_TimeADTGetDatum(newentry->lower)));
	res2 = INTERVAL_TO_SEC(intr);
	res2 = Max(res2, 0);

	res += res2;

	*result = 0.0;

	if (res > 0)
	{
		intr = DatumGetIntervalP(DirectFunctionCall2(
													 time_mi_time,
										 P_TimeADTGetDatum(origentry->upper),
									   P_TimeADTGetDatum(origentry->lower)));
		*result += FLT_MIN;
		*result += (float) (res / (res + INTERVAL_TO_SEC(intr)));
		*result *= (FLT_MAX / (((GISTENTRY *) PG_GETARG_POINTER(0))->rel->rd_att->natts + 1));
	}

	PG_RETURN_POINTER(result);
}


Datum
gbt_time_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_time_same(PG_FUNCTION_ARGS)
{
	timeKEY    *b1 = (timeKEY *) PG_GETARG_POINTER(0);
	timeKEY    *b2 = (timeKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
