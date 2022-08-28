/*
 * contrib/btree_gist/btree_utils_num.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/cash.h"
#include "utils/date.h"
#include "utils/timestamp.h"


GISTENTRY *
gbt_num_compress(GISTENTRY *entry, const gbtree_ninfo *tinfo)
{
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		union
		{
			bool		bo;
			int16		i2;
			int32		i4;
			int64		i8;
			float4		f4;
			float8		f8;
			DateADT		dt;
			TimeADT		tm;
			Timestamp	ts;
			Cash		ch;
		}			v;

		GBT_NUMKEY *r = (GBT_NUMKEY *) palloc0(tinfo->indexsize);
		void	   *leaf = NULL;

		switch (tinfo->t)
		{
			case gbt_t_bool:
				v.bo = DatumGetBool(entry->key);
				leaf = &v.bo;
				break;
			case gbt_t_int2:
				v.i2 = DatumGetInt16(entry->key);
				leaf = &v.i2;
				break;
			case gbt_t_int4:
				v.i4 = DatumGetInt32(entry->key);
				leaf = &v.i4;
				break;
			case gbt_t_int8:
				v.i8 = DatumGetInt64(entry->key);
				leaf = &v.i8;
				break;
			case gbt_t_oid:
			case gbt_t_enum:
				v.i4 = DatumGetObjectId(entry->key);
				leaf = &v.i4;
				break;
			case gbt_t_float4:
				v.f4 = DatumGetFloat4(entry->key);
				leaf = &v.f4;
				break;
			case gbt_t_float8:
				v.f8 = DatumGetFloat8(entry->key);
				leaf = &v.f8;
				break;
			case gbt_t_date:
				v.dt = DatumGetDateADT(entry->key);
				leaf = &v.dt;
				break;
			case gbt_t_time:
				v.tm = DatumGetTimeADT(entry->key);
				leaf = &v.tm;
				break;
			case gbt_t_ts:
				v.ts = DatumGetTimestamp(entry->key);
				leaf = &v.ts;
				break;
			case gbt_t_cash:
				v.ch = DatumGetCash(entry->key);
				leaf = &v.ch;
				break;
			default:
				leaf = DatumGetPointer(entry->key);
		}

		Assert(tinfo->indexsize >= 2 * tinfo->size);

		memcpy((void *) &r[0], leaf, tinfo->size);
		memcpy((void *) &r[tinfo->size], leaf, tinfo->size);
		retval = palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(r), entry->rel, entry->page,
					  entry->offset, false);
	}
	else
		retval = entry;

	return retval;
}

/*
 * Convert a compressed leaf item back to the original type, for index-only
 * scans.
 */
GISTENTRY *
gbt_num_fetch(GISTENTRY *entry, const gbtree_ninfo *tinfo)
{
	GISTENTRY  *retval;
	Datum		datum;

	Assert(tinfo->indexsize >= 2 * tinfo->size);

	/*
	 * Get the original Datum from the stored datum. On leaf entries, the
	 * lower and upper bound are the same. We just grab the lower bound and
	 * return it.
	 */
	switch (tinfo->t)
	{
		case gbt_t_bool:
			datum = BoolGetDatum(*(bool *) entry->key);
			break;
		case gbt_t_int2:
			datum = Int16GetDatum(*(int16 *) entry->key);
			break;
		case gbt_t_int4:
			datum = Int32GetDatum(*(int32 *) entry->key);
			break;
		case gbt_t_int8:
			datum = Int64GetDatum(*(int64 *) entry->key);
			break;
		case gbt_t_oid:
		case gbt_t_enum:
			datum = ObjectIdGetDatum(*(Oid *) entry->key);
			break;
		case gbt_t_float4:
			datum = Float4GetDatum(*(float4 *) entry->key);
			break;
		case gbt_t_float8:
			datum = Float8GetDatum(*(float8 *) entry->key);
			break;
		case gbt_t_date:
			datum = DateADTGetDatum(*(DateADT *) entry->key);
			break;
		case gbt_t_time:
			datum = TimeADTGetDatum(*(TimeADT *) entry->key);
			break;
		case gbt_t_ts:
			datum = TimestampGetDatum(*(Timestamp *) entry->key);
			break;
		case gbt_t_cash:
			datum = CashGetDatum(*(Cash *) entry->key);
			break;
		default:
			datum = entry->key;
	}

	retval = palloc(sizeof(GISTENTRY));
	gistentryinit(*retval, datum, entry->rel, entry->page, entry->offset,
				  false);
	return retval;
}



/*
** The GiST union method for numerical values
*/

void *
gbt_num_union(GBT_NUMKEY *out, const GistEntryVector *entryvec, const gbtree_ninfo *tinfo, FmgrInfo *flinfo)
{
	int			i,
				numranges;
	GBT_NUMKEY *cur;
	GBT_NUMKEY_R o,
				c;

	numranges = entryvec->n;
	cur = (GBT_NUMKEY *) DatumGetPointer((entryvec->vector[0].key));


	o.lower = &((GBT_NUMKEY *) out)[0];
	o.upper = &((GBT_NUMKEY *) out)[tinfo->size];

	memcpy((void *) out, (void *) cur, 2 * tinfo->size);

	for (i = 1; i < numranges; i++)
	{
		cur = (GBT_NUMKEY *) DatumGetPointer((entryvec->vector[i].key));
		c.lower = &cur[0];
		c.upper = &cur[tinfo->size];
		/* if out->lower > cur->lower, adopt cur as lower */
		if (tinfo->f_gt(o.lower, c.lower, flinfo))
			memcpy(unconstify(GBT_NUMKEY *, o.lower), c.lower, tinfo->size);
		/* if out->upper < cur->upper, adopt cur as upper */
		if (tinfo->f_lt(o.upper, c.upper, flinfo))
			memcpy(unconstify(GBT_NUMKEY *, o.upper), c.upper, tinfo->size);
	}

	return out;
}



/*
** The GiST same method for numerical values
*/

bool
gbt_num_same(const GBT_NUMKEY *a, const GBT_NUMKEY *b, const gbtree_ninfo *tinfo, FmgrInfo *flinfo)
{
	GBT_NUMKEY_R b1,
				b2;

	b1.lower = &(a[0]);
	b1.upper = &(a[tinfo->size]);
	b2.lower = &(b[0]);
	b2.upper = &(b[tinfo->size]);

	return (tinfo->f_eq(b1.lower, b2.lower, flinfo) &&
			tinfo->f_eq(b1.upper, b2.upper, flinfo));
}


void
gbt_num_bin_union(Datum *u, GBT_NUMKEY *e, const gbtree_ninfo *tinfo, FmgrInfo *flinfo)
{
	GBT_NUMKEY_R rd;

	rd.lower = &e[0];
	rd.upper = &e[tinfo->size];

	if (!DatumGetPointer(*u))
	{
		*u = PointerGetDatum(palloc0(tinfo->indexsize));
		memcpy(&(((GBT_NUMKEY *) DatumGetPointer(*u))[0]), rd.lower, tinfo->size);
		memcpy(&(((GBT_NUMKEY *) DatumGetPointer(*u))[tinfo->size]), rd.upper, tinfo->size);
	}
	else
	{
		GBT_NUMKEY_R ur;

		ur.lower = &(((GBT_NUMKEY *) DatumGetPointer(*u))[0]);
		ur.upper = &(((GBT_NUMKEY *) DatumGetPointer(*u))[tinfo->size]);
		if (tinfo->f_gt(ur.lower, rd.lower, flinfo))
			memcpy(unconstify(GBT_NUMKEY *, ur.lower), rd.lower, tinfo->size);
		if (tinfo->f_lt(ur.upper, rd.upper, flinfo))
			memcpy(unconstify(GBT_NUMKEY *, ur.upper), rd.upper, tinfo->size);
	}
}



/*
 * The GiST consistent method
 *
 * Note: we currently assume that no datatypes that use this routine are
 * collation-aware; so we don't bother passing collation through.
 */
bool
gbt_num_consistent(const GBT_NUMKEY_R *key,
				   const void *query,
				   const StrategyNumber *strategy,
				   bool is_leaf,
				   const gbtree_ninfo *tinfo,
				   FmgrInfo *flinfo)
{
	bool		retval;

	switch (*strategy)
	{
		case BTLessEqualStrategyNumber:
			retval = tinfo->f_ge(query, key->lower, flinfo);
			break;
		case BTLessStrategyNumber:
			if (is_leaf)
				retval = tinfo->f_gt(query, key->lower, flinfo);
			else
				retval = tinfo->f_ge(query, key->lower, flinfo);
			break;
		case BTEqualStrategyNumber:
			if (is_leaf)
				retval = tinfo->f_eq(query, key->lower, flinfo);
			else
				retval = (tinfo->f_le(key->lower, query, flinfo) &&
						  tinfo->f_le(query, key->upper, flinfo));
			break;
		case BTGreaterStrategyNumber:
			if (is_leaf)
				retval = tinfo->f_lt(query, key->upper, flinfo);
			else
				retval = tinfo->f_le(query, key->upper, flinfo);
			break;
		case BTGreaterEqualStrategyNumber:
			retval = tinfo->f_le(query, key->upper, flinfo);
			break;
		case BtreeGistNotEqualStrategyNumber:
			retval = (!(tinfo->f_eq(query, key->lower, flinfo) &&
						tinfo->f_eq(query, key->upper, flinfo)));
			break;
		default:
			retval = false;
	}

	return retval;
}


/*
** The GiST distance method (for KNN-Gist)
*/

float8
gbt_num_distance(const GBT_NUMKEY_R *key,
				 const void *query,
				 bool is_leaf,
				 const gbtree_ninfo *tinfo,
				 FmgrInfo *flinfo)
{
	float8		retval;

	if (tinfo->f_dist == NULL)
		elog(ERROR, "KNN search is not supported for btree_gist type %d",
			 (int) tinfo->t);
	if (tinfo->f_le(query, key->lower, flinfo))
		retval = tinfo->f_dist(query, key->lower, flinfo);
	else if (tinfo->f_ge(query, key->upper, flinfo))
		retval = tinfo->f_dist(query, key->upper, flinfo);
	else
		retval = 0.0;

	return retval;
}


GIST_SPLITVEC *
gbt_num_picksplit(const GistEntryVector *entryvec, GIST_SPLITVEC *v,
				  const gbtree_ninfo *tinfo, FmgrInfo *flinfo)
{
	OffsetNumber i,
				maxoff = entryvec->n - 1;
	Nsrt	   *arr;
	int			nbytes;

	arr = (Nsrt *) palloc((maxoff + 1) * sizeof(Nsrt));
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);
	v->spl_ldatum = PointerGetDatum(0);
	v->spl_rdatum = PointerGetDatum(0);
	v->spl_nleft = 0;
	v->spl_nright = 0;

	/* Sort entries */

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		arr[i].t = (GBT_NUMKEY *) DatumGetPointer((entryvec->vector[i].key));
		arr[i].i = i;
	}
	qsort_arg((void *) &arr[FirstOffsetNumber], maxoff - FirstOffsetNumber + 1, sizeof(Nsrt), (qsort_arg_comparator) tinfo->f_cmp, (void *) flinfo);

	/* We do simply create two parts */

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		if (i <= (maxoff - FirstOffsetNumber + 1) / 2)
		{
			gbt_num_bin_union(&v->spl_ldatum, arr[i].t, tinfo, flinfo);
			v->spl_left[v->spl_nleft] = arr[i].i;
			v->spl_nleft++;
		}
		else
		{
			gbt_num_bin_union(&v->spl_rdatum, arr[i].t, tinfo, flinfo);
			v->spl_right[v->spl_nright] = arr[i].i;
			v->spl_nright++;
		}
	}

	return v;
}
