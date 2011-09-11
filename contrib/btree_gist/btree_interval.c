/*
 * contrib/btree_gist/btree_interval.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/timestamp.h"

typedef struct
{
	Interval	lower,
				upper;
} intvKEY;


/*
** Interval ops
*/
PG_FUNCTION_INFO_V1(gbt_intv_compress);
PG_FUNCTION_INFO_V1(gbt_intv_decompress);
PG_FUNCTION_INFO_V1(gbt_intv_union);
PG_FUNCTION_INFO_V1(gbt_intv_picksplit);
PG_FUNCTION_INFO_V1(gbt_intv_consistent);
PG_FUNCTION_INFO_V1(gbt_intv_distance);
PG_FUNCTION_INFO_V1(gbt_intv_penalty);
PG_FUNCTION_INFO_V1(gbt_intv_same);

Datum		gbt_intv_compress(PG_FUNCTION_ARGS);
Datum		gbt_intv_decompress(PG_FUNCTION_ARGS);
Datum		gbt_intv_union(PG_FUNCTION_ARGS);
Datum		gbt_intv_picksplit(PG_FUNCTION_ARGS);
Datum		gbt_intv_consistent(PG_FUNCTION_ARGS);
Datum		gbt_intv_distance(PG_FUNCTION_ARGS);
Datum		gbt_intv_penalty(PG_FUNCTION_ARGS);
Datum		gbt_intv_same(PG_FUNCTION_ARGS);


static bool
gbt_intvgt(const void *a, const void *b)
{
	return DatumGetBool(DirectFunctionCall2(interval_gt, IntervalPGetDatum(a), IntervalPGetDatum(b)));
}

static bool
gbt_intvge(const void *a, const void *b)
{
	return DatumGetBool(DirectFunctionCall2(interval_ge, IntervalPGetDatum(a), IntervalPGetDatum(b)));
}

static bool
gbt_intveq(const void *a, const void *b)
{
	return DatumGetBool(DirectFunctionCall2(interval_eq, IntervalPGetDatum(a), IntervalPGetDatum(b)));
}

static bool
gbt_intvle(const void *a, const void *b)
{
	return DatumGetBool(DirectFunctionCall2(interval_le, IntervalPGetDatum(a), IntervalPGetDatum(b)));
}

static bool
gbt_intvlt(const void *a, const void *b)
{
	return DatumGetBool(DirectFunctionCall2(interval_lt, IntervalPGetDatum(a), IntervalPGetDatum(b)));
}

static int
gbt_intvkey_cmp(const void *a, const void *b)
{
	intvKEY    *ia = (intvKEY *) (((const Nsrt *) a)->t);
	intvKEY    *ib = (intvKEY *) (((const Nsrt *) b)->t);
	int			res;

	res = DatumGetInt32(DirectFunctionCall2(interval_cmp, IntervalPGetDatum(&ia->lower), IntervalPGetDatum(&ib->lower)));
	if (res == 0)
		return DatumGetInt32(DirectFunctionCall2(interval_cmp, IntervalPGetDatum(&ia->upper), IntervalPGetDatum(&ib->upper)));

	return res;
}


static double
intr2num(const Interval *i)
{
	return INTERVAL_TO_SEC(i);
}

static float8
gbt_intv_dist(const void *a, const void *b)
{
	return (float8) Abs(intr2num((const Interval *) a) - intr2num((const Interval *) b));
}

/*
 * INTERVALSIZE should be the actual size-on-disk of an Interval, as shown
 * in pg_type.	This might be less than sizeof(Interval) if the compiler
 * insists on adding alignment padding at the end of the struct.
 */
#define INTERVALSIZE 16

static const gbtree_ninfo tinfo =
{
	gbt_t_intv,
	sizeof(Interval),
	gbt_intvgt,
	gbt_intvge,
	gbt_intveq,
	gbt_intvle,
	gbt_intvlt,
	gbt_intvkey_cmp,
	gbt_intv_dist
};


Interval *
abs_interval(Interval *a)
{
	static Interval zero = {0, 0, 0};

	if (DatumGetBool(DirectFunctionCall2(interval_lt,
										 IntervalPGetDatum(a),
										 IntervalPGetDatum(&zero))))
		a = DatumGetIntervalP(DirectFunctionCall1(interval_um,
												  IntervalPGetDatum(a)));

	return a;
}

PG_FUNCTION_INFO_V1(interval_dist);
Datum		interval_dist(PG_FUNCTION_ARGS);
Datum
interval_dist(PG_FUNCTION_ARGS)
{
	Datum		diff = DirectFunctionCall2(interval_mi,
										   PG_GETARG_DATUM(0),
										   PG_GETARG_DATUM(1));

	PG_RETURN_INTERVAL_P(abs_interval(DatumGetIntervalP(diff)));
}


/**************************************************
 * interval ops
 **************************************************/


Datum
gbt_intv_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = entry;

	if (entry->leafkey || INTERVALSIZE != sizeof(Interval))
	{
		char	   *r = (char *) palloc(2 * INTERVALSIZE);

		retval = palloc(sizeof(GISTENTRY));

		if (entry->leafkey)
		{
			Interval   *key = DatumGetIntervalP(entry->key);

			memcpy((void *) r, (void *) key, INTERVALSIZE);
			memcpy((void *) (r + INTERVALSIZE), (void *) key, INTERVALSIZE);
		}
		else
		{
			intvKEY    *key = (intvKEY *) DatumGetPointer(entry->key);

			memcpy(r, &key->lower, INTERVALSIZE);
			memcpy(r + INTERVALSIZE, &key->upper, INTERVALSIZE);
		}
		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
	}

	PG_RETURN_POINTER(retval);

}

Datum
gbt_intv_decompress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = entry;

	if (INTERVALSIZE != sizeof(Interval))
	{
		intvKEY    *r = palloc(sizeof(intvKEY));
		char	   *key = DatumGetPointer(entry->key);

		retval = palloc(sizeof(GISTENTRY));
		memcpy(&r->lower, key, INTERVALSIZE);
		memcpy(&r->upper, key + INTERVALSIZE, INTERVALSIZE);

		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
	}
	PG_RETURN_POINTER(retval);
}


Datum
gbt_intv_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Interval   *query = PG_GETARG_INTERVAL_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	intvKEY    *kkk = (intvKEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_BOOL(
				   gbt_num_consistent(&key, (void *) query, &strategy, GIST_LEAF(entry), &tinfo)
		);
}


Datum
gbt_intv_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Interval   *query = PG_GETARG_INTERVAL_P(1);

	/* Oid		subtype = PG_GETARG_OID(3); */
	intvKEY    *kkk = (intvKEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_FLOAT8(
			 gbt_num_distance(&key, (void *) query, GIST_LEAF(entry), &tinfo)
		);
}


Datum
gbt_intv_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(intvKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(intvKEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
}


Datum
gbt_intv_penalty(PG_FUNCTION_ARGS)
{
	intvKEY    *origentry = (intvKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	intvKEY    *newentry = (intvKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	double		iorg[2],
				inew[2];

	iorg[0] = intr2num(&origentry->lower);
	iorg[1] = intr2num(&origentry->upper);
	inew[0] = intr2num(&newentry->lower);
	inew[1] = intr2num(&newentry->upper);

	penalty_num(result, iorg[0], iorg[1], inew[0], inew[1]);

	PG_RETURN_POINTER(result);

}

Datum
gbt_intv_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_intv_same(PG_FUNCTION_ARGS)
{
	intvKEY    *b1 = (intvKEY *) PG_GETARG_POINTER(0);
	intvKEY    *b2 = (intvKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
