#include "btree_gist.h"


typedef struct tskey
{
	Timestamp	lower;
	Timestamp	upper;
}	TSKEY;


/*
** tskey in/out
*/
PG_FUNCTION_INFO_V1(tskey_in);
PG_FUNCTION_INFO_V1(tskey_out);
Datum		tskey_in(PG_FUNCTION_ARGS);
Datum		tskey_out(PG_FUNCTION_ARGS);


/*
** timestamp ops
*/
PG_FUNCTION_INFO_V1(gts_compress);
PG_FUNCTION_INFO_V1(gts_union);
PG_FUNCTION_INFO_V1(gts_picksplit);
PG_FUNCTION_INFO_V1(gts_consistent);
PG_FUNCTION_INFO_V1(gts_penalty);
PG_FUNCTION_INFO_V1(gts_same);

Datum		gts_compress(PG_FUNCTION_ARGS);
Datum		gts_union(PG_FUNCTION_ARGS);
Datum		gts_picksplit(PG_FUNCTION_ARGS);
Datum		gts_consistent(PG_FUNCTION_ARGS);
Datum		gts_penalty(PG_FUNCTION_ARGS);
Datum		gts_same(PG_FUNCTION_ARGS);

static void gts_binary_union(Datum *r1, char *r2);
static int	tskey_cmp(const void *a, const void *b);

#define TimestampGetDatumFast(X) Float8GetDatumFast(X)

/* define for comparison */
#define TSGE( ts1, ts2 ) (DatumGetBool(DirectFunctionCall2( \
		timestamp_ge, \
		PointerGetDatum( ts1 ), \
		PointerGetDatum( ts2 ) \
)))
#define TSGT( ts1, ts2 ) (DatumGetBool(DirectFunctionCall2( \
		timestamp_gt, \
		PointerGetDatum( ts1 ), \
		PointerGetDatum( ts2 ) \
)))
#define TSEQ( ts1, ts2 ) (DatumGetBool(DirectFunctionCall2( \
		timestamp_eq, \
		PointerGetDatum( ts1 ), \
		PointerGetDatum( ts2 ) \
)))
#define TSLT( ts1, ts2 ) (DatumGetBool(DirectFunctionCall2( \
		timestamp_lt, \
		PointerGetDatum( ts1 ), \
		PointerGetDatum( ts2 ) \
)))
#define TSLE( ts1, ts2 ) (DatumGetBool(DirectFunctionCall2( \
		timestamp_le, \
		PointerGetDatum( ts1 ), \
		PointerGetDatum( ts2 ) \
)))



/**************************************************
 * timestamp ops
 **************************************************/

Datum
gts_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		TSKEY	   *r = (TSKEY *) palloc(sizeof(TSKEY));

		retval = palloc(sizeof(GISTENTRY));
		r->lower = r->upper = *(Timestamp *) (entry->key);
		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, sizeof(TSKEY), FALSE);
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

Datum
gts_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Timestamp  *query = (Timestamp *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool		retval;
	TSKEY	   *key;

	/*
	 * * if entry is not leaf, use gbox_internal_consistent, * else use
	 * gbox_leaf_consistent
	 */
	if (!entry->key)
		return FALSE;
	key = (TSKEY *) DatumGetPointer(entry->key);

	switch (strategy)
	{
		case BTLessEqualStrategyNumber:
			retval = TSGE(query, &(key->lower));
			break;
		case BTLessStrategyNumber:
			if (GIST_LEAF(entry))
				retval = TSGT(query, &(key->lower));
			else
				retval = TSGE(query, &(key->lower));
			break;
		case BTEqualStrategyNumber:
			/* in leaf page key->lower always = key->upper */
			if (GIST_LEAF(entry))
				retval = TSEQ(query, &(key->lower));
			else
				retval = (TSLE(&(key->lower), query) && TSLE(query, &(key->upper)));
			break;
		case BTGreaterStrategyNumber:
			if (GIST_LEAF(entry))
				retval = TSLT(query, &(key->upper));
			else
				retval = TSLE(query, &(key->upper));
			break;
		case BTGreaterEqualStrategyNumber:
			retval = TSLE(query, &(key->upper));
			break;
		default:
			retval = FALSE;
	}
	PG_RETURN_BOOL(retval);
}

Datum
gts_union(PG_FUNCTION_ARGS)
{
	bytea	   *entryvec = (bytea *) PG_GETARG_POINTER(0);
	int			i,
				numranges;
	TSKEY	   *cur,
			   *out = palloc(sizeof(TSKEY));

	numranges = (VARSIZE(entryvec) - VARHDRSZ) / sizeof(GISTENTRY);
	*(int *) PG_GETARG_POINTER(1) = sizeof(TSKEY);

	cur = (TSKEY *) DatumGetPointer((((GISTENTRY *) (VARDATA(entryvec)))[0].key));
	out->lower = cur->lower;
	out->upper = cur->upper;

	for (i = 1; i < numranges; i++)
	{
		cur = (TSKEY *) DatumGetPointer((((GISTENTRY *) (VARDATA(entryvec)))[i].key));
		if (TSGT(&out->lower, &cur->lower))
			out->lower = cur->lower;
		if (TSLT(&out->upper, &cur->upper))
			out->upper = cur->upper;
	}

	PG_RETURN_POINTER(out);
}

Datum
gts_penalty(PG_FUNCTION_ARGS)
{
	TSKEY	   *origentry = (TSKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	TSKEY	   *newentry = (TSKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	Interval   *intr;

	intr = DatumGetIntervalP(DirectFunctionCall2(
												 timestamp_mi,
								  TimestampGetDatumFast(newentry->upper),
							   TimestampGetDatumFast(origentry->upper)));

	/* see interval_larger */
	*result = Max(intr->time + intr->month * (30.0 * 86400), 0);
	pfree(intr);

	intr = DatumGetIntervalP(DirectFunctionCall2(
												 timestamp_mi,
								 TimestampGetDatumFast(origentry->lower),
								TimestampGetDatumFast(newentry->lower)));

	/* see interval_larger */
	*result += Max(intr->time + intr->month * (30.0 * 86400), 0);
	pfree(intr);

	PG_RETURN_POINTER(result);
}

Datum
gts_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(btree_picksplit(
									  (bytea *) PG_GETARG_POINTER(0),
								  (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
									  gts_binary_union,
									  tskey_cmp
									  ));
}

Datum
gts_same(PG_FUNCTION_ARGS)
{
	TSKEY	   *b1 = (TSKEY *) PG_GETARG_POINTER(0);
	TSKEY	   *b2 = (TSKEY *) PG_GETARG_POINTER(1);

	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	if (b1 && b2)
		*result = (TSEQ(&(b1->lower), &(b2->lower)) && TSEQ(&(b1->upper), &(b2->upper))) ? TRUE : FALSE;
	else
		*result = (b1 == NULL && b2 == NULL) ? TRUE : FALSE;
	PG_RETURN_POINTER(result);
}

static void
gts_binary_union(Datum *r1, char *r2)
{
	TSKEY	   *b1;
	TSKEY	   *b2 = (TSKEY *) r2;

	if (!DatumGetPointer(*r1))
	{
		*r1 = PointerGetDatum(palloc(sizeof(TSKEY)));
		b1 = (TSKEY *) DatumGetPointer(*r1);
		b1->upper = b2->upper;
		b1->lower = b2->lower;
	}
	else
	{
		b1 = (TSKEY *) DatumGetPointer(*r1);

		b1->lower = (TSGT(&b1->lower, &b2->lower)) ?
			b2->lower : b1->lower;
		b1->upper = (TSGT(&b1->upper, &b2->upper)) ?
			b1->upper : b2->upper;
	}
}

static int
tskey_cmp(const void *a, const void *b)
{
	return DatumGetInt32(
						 DirectFunctionCall2(
											 timestamp_cmp,
			  TimestampGetDatumFast(((TSKEY *) (((RIX *) a)->r))->lower),
			   TimestampGetDatumFast(((TSKEY *) (((RIX *) b)->r))->lower)
											 )
		);
}


/**************************************************
 * In/Out for keys, not really needed
 **************************************************/

Datum
tskey_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("feature not implemented")));

	PG_RETURN_POINTER(NULL);
}

Datum
tskey_out(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("feature not implemented")));

	PG_RETURN_POINTER(NULL);
}
