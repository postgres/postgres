/*
 * contrib/btree_gist/btree_ts.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/builtins.h"
#include "utils/datetime.h"

typedef struct
{
	Timestamp	lower;
	Timestamp	upper;
} tsKEY;

/*
** timestamp ops
*/
PG_FUNCTION_INFO_V1(gbt_ts_compress);
PG_FUNCTION_INFO_V1(gbt_tstz_compress);
PG_FUNCTION_INFO_V1(gbt_ts_union);
PG_FUNCTION_INFO_V1(gbt_ts_picksplit);
PG_FUNCTION_INFO_V1(gbt_ts_consistent);
PG_FUNCTION_INFO_V1(gbt_ts_distance);
PG_FUNCTION_INFO_V1(gbt_tstz_consistent);
PG_FUNCTION_INFO_V1(gbt_tstz_distance);
PG_FUNCTION_INFO_V1(gbt_ts_penalty);
PG_FUNCTION_INFO_V1(gbt_ts_same);


#ifdef USE_FLOAT8_BYVAL
#define TimestampGetDatumFast(X) TimestampGetDatum(X)
#else
#define TimestampGetDatumFast(X) PointerGetDatum(&(X))
#endif


static bool
gbt_tsgt(const void *a, const void *b)
{
	const Timestamp *aa = (const Timestamp *) a;
	const Timestamp *bb = (const Timestamp *) b;

	return DatumGetBool(DirectFunctionCall2(timestamp_gt,
											TimestampGetDatumFast(*aa),
											TimestampGetDatumFast(*bb)));
}

static bool
gbt_tsge(const void *a, const void *b)
{
	const Timestamp *aa = (const Timestamp *) a;
	const Timestamp *bb = (const Timestamp *) b;

	return DatumGetBool(DirectFunctionCall2(timestamp_ge,
											TimestampGetDatumFast(*aa),
											TimestampGetDatumFast(*bb)));
}

static bool
gbt_tseq(const void *a, const void *b)
{
	const Timestamp *aa = (const Timestamp *) a;
	const Timestamp *bb = (const Timestamp *) b;

	return DatumGetBool(DirectFunctionCall2(timestamp_eq,
											TimestampGetDatumFast(*aa),
											TimestampGetDatumFast(*bb)));
}

static bool
gbt_tsle(const void *a, const void *b)
{
	const Timestamp *aa = (const Timestamp *) a;
	const Timestamp *bb = (const Timestamp *) b;

	return DatumGetBool(DirectFunctionCall2(timestamp_le,
											TimestampGetDatumFast(*aa),
											TimestampGetDatumFast(*bb)));
}

static bool
gbt_tslt(const void *a, const void *b)
{
	const Timestamp *aa = (const Timestamp *) a;
	const Timestamp *bb = (const Timestamp *) b;

	return DatumGetBool(DirectFunctionCall2(timestamp_lt,
											TimestampGetDatumFast(*aa),
											TimestampGetDatumFast(*bb)));
}


static int
gbt_tskey_cmp(const void *a, const void *b)
{
	tsKEY	   *ia = (tsKEY *) (((const Nsrt *) a)->t);
	tsKEY	   *ib = (tsKEY *) (((const Nsrt *) b)->t);
	int			res;

	res = DatumGetInt32(DirectFunctionCall2(timestamp_cmp, TimestampGetDatumFast(ia->lower), TimestampGetDatumFast(ib->lower)));
	if (res == 0)
		return DatumGetInt32(DirectFunctionCall2(timestamp_cmp, TimestampGetDatumFast(ia->upper), TimestampGetDatumFast(ib->upper)));

	return res;
}

static float8
gbt_ts_dist(const void *a, const void *b)
{
	const Timestamp *aa = (const Timestamp *) a;
	const Timestamp *bb = (const Timestamp *) b;
	Interval   *i;

	if (TIMESTAMP_NOT_FINITE(*aa) || TIMESTAMP_NOT_FINITE(*bb))
		return get_float8_infinity();

	i = DatumGetIntervalP(DirectFunctionCall2(timestamp_mi,
											  TimestampGetDatumFast(*aa),
											  TimestampGetDatumFast(*bb)));
	return (float8) Abs(INTERVAL_TO_SEC(i));
}


static const gbtree_ninfo tinfo =
{
	gbt_t_ts,
	sizeof(Timestamp),
	16,							/* sizeof(gbtreekey16) */
	gbt_tsgt,
	gbt_tsge,
	gbt_tseq,
	gbt_tsle,
	gbt_tslt,
	gbt_tskey_cmp,
	gbt_ts_dist
};


PG_FUNCTION_INFO_V1(ts_dist);
Datum
ts_dist(PG_FUNCTION_ARGS)
{
	Timestamp	a = PG_GETARG_TIMESTAMP(0);
	Timestamp	b = PG_GETARG_TIMESTAMP(1);
	Interval   *r;

	if (TIMESTAMP_NOT_FINITE(a) || TIMESTAMP_NOT_FINITE(b))
	{
		Interval   *p = palloc(sizeof(Interval));

		p->day = INT_MAX;
		p->month = INT_MAX;
#ifdef HAVE_INT64_TIMESTAMP
		p->time = INT64CONST(0x7FFFFFFFFFFFFFFF);
#else
		p->time = DBL_MAX;
#endif
		PG_RETURN_INTERVAL_P(p);
	}
	else
		r = DatumGetIntervalP(DirectFunctionCall2(timestamp_mi,
												  PG_GETARG_DATUM(0),
												  PG_GETARG_DATUM(1)));
	PG_RETURN_INTERVAL_P(abs_interval(r));
}

PG_FUNCTION_INFO_V1(tstz_dist);
Datum
tstz_dist(PG_FUNCTION_ARGS)
{
	TimestampTz a = PG_GETARG_TIMESTAMPTZ(0);
	TimestampTz b = PG_GETARG_TIMESTAMPTZ(1);
	Interval   *r;

	if (TIMESTAMP_NOT_FINITE(a) || TIMESTAMP_NOT_FINITE(b))
	{
		Interval   *p = palloc(sizeof(Interval));

		p->day = INT_MAX;
		p->month = INT_MAX;
#ifdef HAVE_INT64_TIMESTAMP
		p->time = INT64CONST(0x7FFFFFFFFFFFFFFF);
#else
		p->time = DBL_MAX;
#endif
		PG_RETURN_INTERVAL_P(p);
	}

	r = DatumGetIntervalP(DirectFunctionCall2(timestamp_mi,
											  PG_GETARG_DATUM(0),
											  PG_GETARG_DATUM(1)));
	PG_RETURN_INTERVAL_P(abs_interval(r));
}


/**************************************************
 * timestamp ops
 **************************************************/


static inline Timestamp
tstz_to_ts_gmt(TimestampTz ts)
{
	/* No timezone correction is needed, since GMT is offset 0 by definition */
	return (Timestamp) ts;
}


Datum
gbt_ts_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = NULL;

	PG_RETURN_POINTER(gbt_num_compress(retval, entry, &tinfo));
}


Datum
gbt_tstz_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		tsKEY	   *r = (tsKEY *) palloc(sizeof(tsKEY));
		TimestampTz ts = DatumGetTimestampTz(entry->key);
		Timestamp	gmt;

		gmt = tstz_to_ts_gmt(ts);

		retval = palloc(sizeof(GISTENTRY));
		r->lower = r->upper = gmt;
		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
	}
	else
		retval = entry;

	PG_RETURN_POINTER(retval);
}


Datum
gbt_ts_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Timestamp	query = PG_GETARG_TIMESTAMP(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	tsKEY	   *kkk = (tsKEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_BOOL(
				   gbt_num_consistent(&key, (void *) &query, &strategy, GIST_LEAF(entry), &tinfo)
		);
}

Datum
gbt_ts_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Timestamp	query = PG_GETARG_TIMESTAMP(1);

	/* Oid		subtype = PG_GETARG_OID(3); */
	tsKEY	   *kkk = (tsKEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_FLOAT8(
			gbt_num_distance(&key, (void *) &query, GIST_LEAF(entry), &tinfo)
		);
}

Datum
gbt_tstz_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	TimestampTz query = PG_GETARG_TIMESTAMPTZ(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	char	   *kkk = (char *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;
	Timestamp	qqq;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk[0];
	key.upper = (GBT_NUMKEY *) &kkk[MAXALIGN(tinfo.size)];
	qqq = tstz_to_ts_gmt(query);

	PG_RETURN_BOOL(
				   gbt_num_consistent(&key, (void *) &qqq, &strategy, GIST_LEAF(entry), &tinfo)
		);
}

Datum
gbt_tstz_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	TimestampTz query = PG_GETARG_TIMESTAMPTZ(1);

	/* Oid		subtype = PG_GETARG_OID(3); */
	char	   *kkk = (char *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;
	Timestamp	qqq;

	key.lower = (GBT_NUMKEY *) &kkk[0];
	key.upper = (GBT_NUMKEY *) &kkk[MAXALIGN(tinfo.size)];
	qqq = tstz_to_ts_gmt(query);

	PG_RETURN_FLOAT8(
			  gbt_num_distance(&key, (void *) &qqq, GIST_LEAF(entry), &tinfo)
		);
}


Datum
gbt_ts_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(tsKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(tsKEY);
	PG_RETURN_POINTER(gbt_num_union((void *) out, entryvec, &tinfo));
}


#define penalty_check_max_float(val) do { \
		if ( val > FLT_MAX ) \
				val = FLT_MAX; \
		if ( val < -FLT_MAX ) \
				val = -FLT_MAX; \
} while(false);


Datum
gbt_ts_penalty(PG_FUNCTION_ARGS)
{
	tsKEY	   *origentry = (tsKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	tsKEY	   *newentry = (tsKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	double		orgdbl[2],
				newdbl[2];

	/*
	 * We are allways using "double" timestamps here. Precision should be good
	 * enough.
	 */
	orgdbl[0] = ((double) origentry->lower);
	orgdbl[1] = ((double) origentry->upper);
	newdbl[0] = ((double) newentry->lower);
	newdbl[1] = ((double) newentry->upper);

	penalty_check_max_float(orgdbl[0]);
	penalty_check_max_float(orgdbl[1]);
	penalty_check_max_float(newdbl[0]);
	penalty_check_max_float(newdbl[1]);

	penalty_num(result, orgdbl[0], orgdbl[1], newdbl[0], newdbl[1]);

	PG_RETURN_POINTER(result);

}


Datum
gbt_ts_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit(
									(GistEntryVector *) PG_GETARG_POINTER(0),
									  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo
										));
}

Datum
gbt_ts_same(PG_FUNCTION_ARGS)
{
	tsKEY	   *b1 = (tsKEY *) PG_GETARG_POINTER(0);
	tsKEY	   *b2 = (tsKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo);
	PG_RETURN_POINTER(result);
}
