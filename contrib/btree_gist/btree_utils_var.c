/*
 * contrib/btree_gist/btree_utils_var.c
 *
 * Common routines for btree_gist code working with varlena indexed data types.
 */
#include "postgres.h"

#include <limits.h>
#include <float.h>

#include "btree_gist.h"
#include "btree_utils_var.h"
#include "utils/rel.h"
#include "varatt.h"

/* used for key sorting */
typedef struct
{
	int			i;
	GBT_VARKEY *t;
} Vsrt;

typedef struct
{
	const gbtree_vinfo *tinfo;
	Oid			collation;
	FmgrInfo   *flinfo;
} gbt_vsrt_arg;


PG_FUNCTION_INFO_V1(gbt_var_decompress);
PG_FUNCTION_INFO_V1(gbt_var_fetch);


Datum
gbt_var_decompress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GBT_VARKEY *key = (GBT_VARKEY *) PG_DETOAST_DATUM(entry->key);

	/* We only need a new GISTENTRY if detoasting did something */
	if (key != (GBT_VARKEY *) DatumGetPointer(entry->key))
	{
		GISTENTRY  *retval = palloc_object(GISTENTRY);

		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, false);

		PG_RETURN_POINTER(retval);
	}

	PG_RETURN_POINTER(entry);
}

/*
 * Extract a "readable" representation of a GBT_VARKEY, containing direct
 * pointers to the contained lower and upper datums.
 */
GBT_VARKEY_R
gbt_var_key_readable(const GBT_VARKEY *k)
{
	GBT_VARKEY_R r;

	r.lower = (const bytea *) &(((const char *) k)[VARHDRSZ]);
	if (VARSIZE(k) > (VARHDRSZ + (VARSIZE(r.lower))))
		r.upper = (const bytea *) &(((const char *) k)[VARHDRSZ + INTALIGN(VARSIZE(r.lower))]);
	else
		r.upper = r.lower;
	return r;
}


/*
 * Create a leaf-entry to store in the index, from a single Datum.
 */
static GBT_VARKEY *
gbt_var_key_from_datum(const varlena *u)
{
	int32		lowersize = VARSIZE(u);
	GBT_VARKEY *r;

	r = (GBT_VARKEY *) palloc(lowersize + VARHDRSZ);
	memcpy(VARDATA(r), u, lowersize);
	SET_VARSIZE(r, lowersize + VARHDRSZ);

	return r;
}

/*
 * Create a key entry to store in the index, from lower and upper bound.
 *
 * This code assumes that none of the types we work with require more
 * than INTALIGN alignment.
 */
GBT_VARKEY *
gbt_var_key_copy(const GBT_VARKEY_R *u)
{
	int32		lowersize = VARSIZE(u->lower);
	int32		uppersize = VARSIZE(u->upper);
	GBT_VARKEY *r;

	r = (GBT_VARKEY *) palloc0(INTALIGN(lowersize) + uppersize + VARHDRSZ);
	memcpy(VARDATA(r), u->lower, lowersize);
	memcpy(VARDATA(r) + INTALIGN(lowersize), u->upper, uppersize);
	SET_VARSIZE(r, INTALIGN(lowersize) + uppersize + VARHDRSZ);

	return r;
}

/*
 * Convert a GBT_VARKEY in leaf form to a GBT_VARKEY in internal form.
 * No-op if the data type doesn't require a transformation.
 */
static GBT_VARKEY *
gbt_var_leaf2node(GBT_VARKEY *leaf, const gbtree_vinfo *tinfo, FmgrInfo *flinfo)
{
	if (tinfo->f_l2n)
		return tinfo->f_l2n(leaf, flinfo);
	else
		return leaf;
}


/*
 * returns the common prefix length of a node key
 *
 * This is not used for any cases where the underlying type is character data
 * (except in gbt_var_penalty, where it doesn't matter since we're just making
 * an estimated correction not constructing a truncated string).  If it were,
 * we'd need to be careful not to truncate in the middle of a multibyte
 * character.
 */
static int32
gbt_var_node_cp_len(const GBT_VARKEY *node, const gbtree_vinfo *tinfo)
{
	GBT_VARKEY_R r = gbt_var_key_readable(node);
	int32		i = 0;
	int32		t1len = VARSIZE(r.lower) - VARHDRSZ;
	int32		t2len = VARSIZE(r.upper) - VARHDRSZ;
	int32		ml = Min(t1len, t2len);
	char	   *p1 = VARDATA(r.lower);
	char	   *p2 = VARDATA(r.upper);

	if (ml == 0)
		return 0;

	while (i < ml)
	{
		if (*p1 != *p2)
			return i;
		p1++;
		p2++;
		i++;
	}
	return ml;					/* lower == upper */
}


/*
 * returns true if query matches prefix up to the length of the prefix
 *
 * We need this to avoid edge-case problems when the "prefix" is a truncated
 * datum; see discussion in btree_utils_var.h.
 */
static bool
gbt_bytea_pf_match(const bytea *pf, const bytea *query, const gbtree_vinfo *tinfo)
{
	bool		out = false;
	int32		qlen = VARSIZE(query) - VARHDRSZ;
	int32		nlen = VARSIZE(pf) - VARHDRSZ;

	if (nlen <= qlen)
	{
		char	   *q = VARDATA(query);
		char	   *n = VARDATA(pf);

		out = (memcmp(q, n, nlen) == 0);
	}

	return out;
}


/*
 * returns true if query matches node according to common-prefix rule
 *
 * If the data type is truncatable, then a shortened upper bound must be
 * considered to include all values that match it up to its own length,
 * even though longer values would normally be considered larger.
 * We don't need to check the lower bound though: shortening it just
 * makes it even smaller.
 */
static bool
gbt_var_node_pf_match(const GBT_VARKEY_R *node, const bytea *query, const gbtree_vinfo *tinfo)
{
	return (tinfo->trnc &&
			gbt_bytea_pf_match(node->upper, query, tinfo));
}


/*
 * truncates / compresses the node key
 *
 * cpf_length is the common prefix length of the lower and upper values.
 * We truncate to that plus one byte, so that the node represents a range
 * of leaf values but doesn't have undue specificity.
 */
static GBT_VARKEY *
gbt_var_node_truncate(const GBT_VARKEY *node, int32 cpf_length, const gbtree_vinfo *tinfo)
{
	GBT_VARKEY *out;
	GBT_VARKEY_R r = gbt_var_key_readable(node);
	int32		len1 = VARSIZE(r.lower) - VARHDRSZ;
	int32		len2 = VARSIZE(r.upper) - VARHDRSZ;
	int32		si;
	char	   *out2;

	len1 = Min(len1, (cpf_length + 1));
	len2 = Min(len2, (cpf_length + 1));

	si = 2 * VARHDRSZ + INTALIGN(len1 + VARHDRSZ) + len2;
	out = (GBT_VARKEY *) palloc0(si);
	SET_VARSIZE(out, si);

	memcpy(VARDATA(out), r.lower, len1 + VARHDRSZ);
	SET_VARSIZE(VARDATA(out), len1 + VARHDRSZ);

	out2 = VARDATA(out) + INTALIGN(len1 + VARHDRSZ);
	memcpy(out2, r.upper, len2 + VARHDRSZ);
	SET_VARSIZE(out2, len2 + VARHDRSZ);

	return out;
}



void
gbt_var_bin_union(Datum *u, GBT_VARKEY *e, Oid collation,
				  const gbtree_vinfo *tinfo, FmgrInfo *flinfo)
{
	GBT_VARKEY_R eo = gbt_var_key_readable(e);
	GBT_VARKEY_R nr;

	if (eo.lower == eo.upper)	/* if leaf, convert to internal form */
	{
		GBT_VARKEY *tmp;

		tmp = gbt_var_leaf2node(e, tinfo, flinfo);
		if (tmp != e)
			eo = gbt_var_key_readable(tmp);
	}

	if (DatumGetPointer(*u))
	{
		GBT_VARKEY_R ro = gbt_var_key_readable((GBT_VARKEY *) DatumGetPointer(*u));
		bool		update = false;

		nr.lower = ro.lower;
		nr.upper = ro.upper;

		if (tinfo->f_cmp(ro.lower, eo.lower, collation, flinfo) > 0)
		{
			nr.lower = eo.lower;
			update = true;
		}

		if (tinfo->f_cmp(ro.upper, eo.upper, collation, flinfo) < 0)
		{
			nr.upper = eo.upper;
			update = true;
		}

		if (update)
			*u = PointerGetDatum(gbt_var_key_copy(&nr));
	}
	else
	{
		nr.lower = eo.lower;
		nr.upper = eo.upper;
		*u = PointerGetDatum(gbt_var_key_copy(&nr));
	}
}


GISTENTRY *
gbt_var_compress(GISTENTRY *entry, const gbtree_vinfo *tinfo)
{
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		varlena    *leaf = PG_DETOAST_DATUM(entry->key);
		GBT_VARKEY *r;

		r = gbt_var_key_from_datum(leaf);

		retval = palloc_object(GISTENTRY);
		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, true);
	}
	else
		retval = entry;

	return retval;
}


Datum
gbt_var_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GBT_VARKEY *key = (GBT_VARKEY *) PG_DETOAST_DATUM(entry->key);
	GBT_VARKEY_R r = gbt_var_key_readable(key);
	GISTENTRY  *retval;

	retval = palloc_object(GISTENTRY);
	gistentryinit(*retval, PointerGetDatum(r.lower),
				  entry->rel, entry->page,
				  entry->offset, true);

	PG_RETURN_POINTER(retval);
}


GBT_VARKEY *
gbt_var_union(const GistEntryVector *entryvec, int32 *size, Oid collation,
			  const gbtree_vinfo *tinfo, FmgrInfo *flinfo)
{
	int			i = 0,
				numranges = entryvec->n;
	GBT_VARKEY *cur;
	Datum		out;
	GBT_VARKEY_R rk;

	*size = sizeof(GBT_VARKEY);

	cur = (GBT_VARKEY *) DatumGetPointer(entryvec->vector[0].key);
	rk = gbt_var_key_readable(cur);
	out = PointerGetDatum(gbt_var_key_copy(&rk));

	for (i = 1; i < numranges; i++)
	{
		cur = (GBT_VARKEY *) DatumGetPointer(entryvec->vector[i].key);
		gbt_var_bin_union(&out, cur, collation, tinfo, flinfo);
	}


	/* Truncate (=compress) key */
	if (tinfo->trnc)
	{
		int32		plen;
		GBT_VARKEY *trc;

		plen = gbt_var_node_cp_len((GBT_VARKEY *) DatumGetPointer(out), tinfo);
		trc = gbt_var_node_truncate((GBT_VARKEY *) DatumGetPointer(out), plen + 1, tinfo);

		out = PointerGetDatum(trc);
	}

	return ((GBT_VARKEY *) DatumGetPointer(out));
}


bool
gbt_var_same(Datum d1, Datum d2, Oid collation,
			 const gbtree_vinfo *tinfo, FmgrInfo *flinfo)
{
	GBT_VARKEY *t1 = (GBT_VARKEY *) DatumGetPointer(d1);
	GBT_VARKEY *t2 = (GBT_VARKEY *) DatumGetPointer(d2);
	GBT_VARKEY_R r1,
				r2;

	r1 = gbt_var_key_readable(t1);
	r2 = gbt_var_key_readable(t2);

	return (tinfo->f_cmp(r1.lower, r2.lower, collation, flinfo) == 0 &&
			tinfo->f_cmp(r1.upper, r2.upper, collation, flinfo) == 0);
}


float *
gbt_var_penalty(float *res, const GISTENTRY *o, const GISTENTRY *n,
				Oid collation, const gbtree_vinfo *tinfo, FmgrInfo *flinfo)
{
	GBT_VARKEY *orge = (GBT_VARKEY *) DatumGetPointer(o->key);
	GBT_VARKEY *newe = (GBT_VARKEY *) DatumGetPointer(n->key);
	GBT_VARKEY_R ok,
				nk;

	*res = 0.0;

	nk = gbt_var_key_readable(newe);
	if (nk.lower == nk.upper)	/* if leaf, convert to internal form */
	{
		GBT_VARKEY *tmp;

		tmp = gbt_var_leaf2node(newe, tinfo, flinfo);
		if (tmp != newe)
			nk = gbt_var_key_readable(tmp);
	}
	ok = gbt_var_key_readable(orge);

	if ((VARSIZE(ok.lower) - VARHDRSZ) == 0 && (VARSIZE(ok.upper) - VARHDRSZ) == 0)
		*res = 0.0;
	else if (!((tinfo->f_cmp(nk.lower, ok.lower, collation, flinfo) >= 0 ||
				gbt_bytea_pf_match(ok.lower, nk.lower, tinfo)) &&
			   (tinfo->f_cmp(nk.upper, ok.upper, collation, flinfo) <= 0 ||
				gbt_bytea_pf_match(ok.upper, nk.upper, tinfo))))
	{
		Datum		d = PointerGetDatum(0);
		double		dres;
		int32		ol,
					ul;

		gbt_var_bin_union(&d, orge, collation, tinfo, flinfo);
		ol = gbt_var_node_cp_len((GBT_VARKEY *) DatumGetPointer(d), tinfo);
		gbt_var_bin_union(&d, newe, collation, tinfo, flinfo);
		ul = gbt_var_node_cp_len((GBT_VARKEY *) DatumGetPointer(d), tinfo);

		if (ul < ol)
		{
			dres = (ol - ul);	/* reduction of common prefix len */
		}
		else
		{
			GBT_VARKEY_R uk = gbt_var_key_readable((GBT_VARKEY *) DatumGetPointer(d));
			unsigned char tmp[4];

			tmp[0] = (unsigned char) (((VARSIZE(ok.lower) - VARHDRSZ) <= ul) ? 0 : (VARDATA(ok.lower)[ul]));
			tmp[1] = (unsigned char) (((VARSIZE(uk.lower) - VARHDRSZ) <= ul) ? 0 : (VARDATA(uk.lower)[ul]));
			tmp[2] = (unsigned char) (((VARSIZE(ok.upper) - VARHDRSZ) <= ul) ? 0 : (VARDATA(ok.upper)[ul]));
			tmp[3] = (unsigned char) (((VARSIZE(uk.upper) - VARHDRSZ) <= ul) ? 0 : (VARDATA(uk.upper)[ul]));
			dres = abs(tmp[0] - tmp[1]) + abs(tmp[3] - tmp[2]);
			dres /= 256.0;
		}

		*res += FLT_MIN;
		*res += (float) (dres / ((double) (ol + 1)));
		*res *= (FLT_MAX / (o->rel->rd_att->natts + 1));
	}

	return res;
}


static int
gbt_vsrt_cmp(const void *a, const void *b, void *arg)
{
	GBT_VARKEY_R ar = gbt_var_key_readable(((const Vsrt *) a)->t);
	GBT_VARKEY_R br = gbt_var_key_readable(((const Vsrt *) b)->t);
	const gbt_vsrt_arg *varg = (const gbt_vsrt_arg *) arg;
	int			res;

	res = varg->tinfo->f_cmp(ar.lower, br.lower, varg->collation, varg->flinfo);
	if (res == 0)
		return varg->tinfo->f_cmp(ar.upper, br.upper, varg->collation, varg->flinfo);

	return res;
}

GIST_SPLITVEC *
gbt_var_picksplit(const GistEntryVector *entryvec, GIST_SPLITVEC *v,
				  Oid collation, const gbtree_vinfo *tinfo, FmgrInfo *flinfo)
{
	OffsetNumber i,
				maxoff = entryvec->n - 1;
	Vsrt	   *arr;
	int			svcntr = 0,
				nbytes;
	char	   *cur;
	GBT_VARKEY **sv = NULL;
	gbt_vsrt_arg varg;

	arr = palloc_array(Vsrt, maxoff + 1);
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);
	v->spl_ldatum = PointerGetDatum(0);
	v->spl_rdatum = PointerGetDatum(0);
	v->spl_nleft = 0;
	v->spl_nright = 0;

	sv = palloc_array(GBT_VARKEY *, maxoff + 1);

	/* Sort entries */

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		GBT_VARKEY_R ro;

		cur = (char *) DatumGetPointer(entryvec->vector[i].key);
		ro = gbt_var_key_readable((GBT_VARKEY *) cur);
		if (ro.lower == ro.upper)	/* leaf */
		{
			sv[svcntr] = gbt_var_leaf2node((GBT_VARKEY *) cur, tinfo, flinfo);
			arr[i].t = sv[svcntr];
			if (sv[svcntr] != (GBT_VARKEY *) cur)
				svcntr++;
		}
		else
			arr[i].t = (GBT_VARKEY *) cur;
		arr[i].i = i;
	}

	/* sort */
	varg.tinfo = tinfo;
	varg.collation = collation;
	varg.flinfo = flinfo;
	qsort_arg(&arr[FirstOffsetNumber],
			  maxoff - FirstOffsetNumber + 1,
			  sizeof(Vsrt),
			  gbt_vsrt_cmp,
			  &varg);

	/* We do simply create two parts */

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		if (i <= (maxoff - FirstOffsetNumber + 1) / 2)
		{
			gbt_var_bin_union(&v->spl_ldatum, arr[i].t, collation, tinfo, flinfo);
			v->spl_left[v->spl_nleft] = arr[i].i;
			v->spl_nleft++;
		}
		else
		{
			gbt_var_bin_union(&v->spl_rdatum, arr[i].t, collation, tinfo, flinfo);
			v->spl_right[v->spl_nright] = arr[i].i;
			v->spl_nright++;
		}
	}

	/* Truncate (=compress) key */
	if (tinfo->trnc)
	{
		int32		ll = gbt_var_node_cp_len((GBT_VARKEY *) DatumGetPointer(v->spl_ldatum), tinfo);
		int32		lr = gbt_var_node_cp_len((GBT_VARKEY *) DatumGetPointer(v->spl_rdatum), tinfo);
		GBT_VARKEY *dl;
		GBT_VARKEY *dr;

		ll = Max(ll, lr);
		ll++;

		dl = gbt_var_node_truncate((GBT_VARKEY *) DatumGetPointer(v->spl_ldatum), ll, tinfo);
		dr = gbt_var_node_truncate((GBT_VARKEY *) DatumGetPointer(v->spl_rdatum), ll, tinfo);
		v->spl_ldatum = PointerGetDatum(dl);
		v->spl_rdatum = PointerGetDatum(dr);
	}

	return v;
}


/*
 * The GiST consistent method
 */
bool
gbt_var_consistent(const GBT_VARKEY_R *key,
				   const void *query,
				   StrategyNumber strategy,
				   Oid collation,
				   bool is_leaf,
				   const gbtree_vinfo *tinfo,
				   FmgrInfo *flinfo)
{
	bool		retval;

	/*
	 * On leaf pages we directly apply the check "key->lower OP query"; we
	 * need not consider key->upper since it will be equal to key->lower.
	 *
	 * On internal pages we mostly need to check "is lower bound below query?"
	 * and/or "is upper bound above query?", where we must allow equality in
	 * both cases.  When checking against the upper bound we have to allow a
	 * gbt_var_node_pf_match too.
	 *
	 * Remember that f_cmp is for internal pages, f_eq etc for leaf pages.
	 */
#define lower_is_below_query() \
	(tinfo->f_cmp(key->lower, query, collation, flinfo) <= 0)
#define upper_is_above_query() \
	(tinfo->f_cmp(key->upper, query, collation, flinfo) >= 0 || \
	 gbt_var_node_pf_match(key, query, tinfo))

	switch (strategy)
	{
		case BTLessEqualStrategyNumber:
			if (is_leaf)
				retval = tinfo->f_le(key->lower, query, collation, flinfo);
			else
				retval = lower_is_below_query();
			break;
		case BTLessStrategyNumber:
			if (is_leaf)
				retval = tinfo->f_lt(key->lower, query, collation, flinfo);
			else
				retval = lower_is_below_query();
			break;
		case BTEqualStrategyNumber:
			if (is_leaf)
				retval = tinfo->f_eq(key->lower, query, collation, flinfo);
			else
				retval = lower_is_below_query() && upper_is_above_query();
			break;
		case BTGreaterStrategyNumber:
			if (is_leaf)
				retval = tinfo->f_gt(key->lower, query, collation, flinfo);
			else
				retval = upper_is_above_query();
			break;
		case BTGreaterEqualStrategyNumber:
			if (is_leaf)
				retval = tinfo->f_ge(key->lower, query, collation, flinfo);
			else
				retval = upper_is_above_query();
			break;
		case BtreeGistNotEqualStrategyNumber:
			if (is_leaf)
				retval = !(tinfo->f_eq(key->lower, query, collation, flinfo));
			else
			{
				/*
				 * If the upper/lower bounds are equal and not truncated, then
				 * all entries below this node must have exactly that value.
				 * So we can avoid descending if the query equals both bounds.
				 * In all other cases, we must descend.
				 */
				retval = tinfo->trnc ||
					!(tinfo->f_cmp(key->lower, query, collation, flinfo) == 0 &&
					  tinfo->f_cmp(key->upper, query, collation, flinfo) == 0);
			}
			break;
		default:
			retval = false;
	}

#undef lower_is_below_query
#undef upper_is_above_query

	return retval;
}
