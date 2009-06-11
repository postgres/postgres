/*
 * $PostgreSQL: pgsql/contrib/btree_gist/btree_utils_num.c,v 1.12 2009/06/11 14:48:50 momjian Exp $
 */
#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/cash.h"
#include "utils/date.h"


GISTENTRY *
gbt_num_compress(GISTENTRY *retval, GISTENTRY *entry, const gbtree_ninfo *tinfo)
{
	if (entry->leafkey)
	{
		union
		{
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

		GBT_NUMKEY *r = (GBT_NUMKEY *) palloc0(2 * tinfo->size);
		void	   *leaf = NULL;

		switch (tinfo->t)
		{
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

		memcpy((void *) &r[0], leaf, tinfo->size);
		memcpy((void *) &r[tinfo->size], leaf, tinfo->size);
		retval = palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(r), entry->rel, entry->page,
					  entry->offset, FALSE);
	}
	else
		retval = entry;

	return retval;
}




/*
** The GiST union method for numerical values
*/

void *
gbt_num_union(GBT_NUMKEY *out, const GistEntryVector *entryvec, const gbtree_ninfo *tinfo)
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
		if ((*tinfo->f_gt) (o.lower, c.lower))	/* out->lower > cur->lower */
			memcpy((void *) o.lower, (void *) c.lower, tinfo->size);
		if ((*tinfo->f_lt) (o.upper, c.upper))	/* out->upper < cur->upper */
			memcpy((void *) o.upper, (void *) c.upper, tinfo->size);
	}

	return out;
}



/*
** The GiST same method for numerical values
*/

bool
gbt_num_same(const GBT_NUMKEY *a, const GBT_NUMKEY *b, const gbtree_ninfo *tinfo)
{

	GBT_NUMKEY_R b1,
				b2;

	b1.lower = &(((GBT_NUMKEY *) a)[0]);
	b1.upper = &(((GBT_NUMKEY *) a)[tinfo->size]);
	b2.lower = &(((GBT_NUMKEY *) b)[0]);
	b2.upper = &(((GBT_NUMKEY *) b)[tinfo->size]);

	if (
		(*tinfo->f_eq) (b1.lower, b2.lower) &&
		(*tinfo->f_eq) (b1.upper, b2.upper)
		)
		return TRUE;
	return FALSE;

}


void
gbt_num_bin_union(Datum *u, GBT_NUMKEY *e, const gbtree_ninfo *tinfo)
{

	GBT_NUMKEY_R rd;

	rd.lower = &e[0];
	rd.upper = &e[tinfo->size];

	if (!DatumGetPointer(*u))
	{
		*u = PointerGetDatum(palloc(2 * tinfo->size));
		memcpy((void *) &(((GBT_NUMKEY *) DatumGetPointer(*u))[0]), (void *) rd.lower, tinfo->size);
		memcpy((void *) &(((GBT_NUMKEY *) DatumGetPointer(*u))[tinfo->size]), (void *) rd.upper, tinfo->size);
	}
	else
	{
		GBT_NUMKEY_R ur;

		ur.lower = &(((GBT_NUMKEY *) DatumGetPointer(*u))[0]);
		ur.upper = &(((GBT_NUMKEY *) DatumGetPointer(*u))[tinfo->size]);
		if ((*tinfo->f_gt) ((void *) ur.lower, (void *) rd.lower))
			memcpy((void *) ur.lower, (void *) rd.lower, tinfo->size);
		if ((*tinfo->f_lt) ((void *) ur.upper, (void *) rd.upper))
			memcpy((void *) ur.upper, (void *) rd.upper, tinfo->size);
	}
}



/*
** The GiST consistent method
*/

bool
gbt_num_consistent(
				   const GBT_NUMKEY_R *key,
				   const void *query,
				   const StrategyNumber *strategy,
				   bool is_leaf,
				   const gbtree_ninfo *tinfo
)
{

	bool		retval = FALSE;

	switch (*strategy)
	{
		case BTLessEqualStrategyNumber:
			retval = (*tinfo->f_ge) (query, key->lower);
			break;
		case BTLessStrategyNumber:
			if (is_leaf)
				retval = (*tinfo->f_gt) (query, key->lower);
			else
				retval = (*tinfo->f_ge) (query, key->lower);
			break;
		case BTEqualStrategyNumber:
			if (is_leaf)
				retval = (*tinfo->f_eq) (query, key->lower);
			else
				retval = (*tinfo->f_le) (key->lower, query) && (*tinfo->f_le) (query, key->upper);
			break;
		case BTGreaterStrategyNumber:
			if (is_leaf)
				retval = (*tinfo->f_lt) (query, key->upper);
			else
				retval = (*tinfo->f_le) (query, key->upper);
			break;
		case BTGreaterEqualStrategyNumber:
			retval = (*tinfo->f_le) (query, key->upper);
			break;
		default:
			retval = FALSE;
	}

	return (retval);
}


GIST_SPLITVEC *
gbt_num_picksplit(const GistEntryVector *entryvec, GIST_SPLITVEC *v,
				  const gbtree_ninfo *tinfo)
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
	qsort((void *) &arr[FirstOffsetNumber], maxoff - FirstOffsetNumber + 1, sizeof(Nsrt), tinfo->f_cmp);

	/* We do simply create two parts */

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		if (i <= (maxoff - FirstOffsetNumber + 1) / 2)
		{
			gbt_num_bin_union(&v->spl_ldatum, arr[i].t, tinfo);
			v->spl_left[v->spl_nleft] = arr[i].i;
			v->spl_nleft++;
		}
		else
		{
			gbt_num_bin_union(&v->spl_rdatum, arr[i].t, tinfo);
			v->spl_right[v->spl_nright] = arr[i].i;
			v->spl_nright++;
		}
	}

	return v;
}
