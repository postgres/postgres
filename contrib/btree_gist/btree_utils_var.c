/*
 * $PostgreSQL: pgsql/contrib/btree_gist/btree_utils_var.c,v 1.21 2009/06/11 14:48:50 momjian Exp $
 */
#include "btree_gist.h"

#include <math.h>
#include <limits.h>
#include <float.h>

#include "btree_utils_var.h"
#include "utils/pg_locale.h"
#include "utils/builtins.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(gbt_var_decompress);
Datum		gbt_var_decompress(PG_FUNCTION_ARGS);


Datum
gbt_var_decompress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GBT_VARKEY *key = (GBT_VARKEY *) DatumGetPointer(PG_DETOAST_DATUM(entry->key));

	if (key != (GBT_VARKEY *) DatumGetPointer(entry->key))
	{
		GISTENTRY  *retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));

		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, FALSE);

		PG_RETURN_POINTER(retval);
	}

	PG_RETURN_POINTER(entry);
}

/* Returns a better readable representaion of variable key ( sets pointer ) */
GBT_VARKEY_R
gbt_var_key_readable(const GBT_VARKEY *k)
{

	GBT_VARKEY_R r;

	r.lower = (bytea *) &(((char *) k)[VARHDRSZ]);
	if (VARSIZE(k) > (VARHDRSZ + (VARSIZE(r.lower))))
		r.upper = (bytea *) &(((char *) k)[VARHDRSZ + INTALIGN(VARSIZE(r.lower))]);
	else
		r.upper = r.lower;
	return r;
}


GBT_VARKEY *
gbt_var_key_copy(const GBT_VARKEY_R *u, bool force_node)
{
	GBT_VARKEY *r = NULL;

	if (u->lower == u->upper && !force_node)
	{							/* leaf key mode */
		r = (GBT_VARKEY *) palloc(VARSIZE(u->lower) + VARHDRSZ);
		memcpy(VARDATA(r), u->lower, VARSIZE(u->lower));
		SET_VARSIZE(r, VARSIZE(u->lower) + VARHDRSZ);
	}
	else
	{							/* node key mode  */
		r = (GBT_VARKEY *) palloc(INTALIGN(VARSIZE(u->lower)) + VARSIZE(u->upper) + VARHDRSZ);
		memcpy(VARDATA(r), u->lower, VARSIZE(u->lower));
		memcpy(VARDATA(r) + INTALIGN(VARSIZE(u->lower)), u->upper, VARSIZE(u->upper));
		SET_VARSIZE(r, INTALIGN(VARSIZE(u->lower)) + VARSIZE(u->upper) + VARHDRSZ);
	}
	return r;
}


static GBT_VARKEY *
gbt_var_leaf2node(GBT_VARKEY *leaf, const gbtree_vinfo *tinfo)
{
	GBT_VARKEY *out = leaf;

	if (tinfo->f_l2n)
		out = (*tinfo->f_l2n) (leaf);

	return out;
}


/*
 * returns the common prefix length of a node key
*/
static int32
gbt_var_node_cp_len(const GBT_VARKEY *node, const gbtree_vinfo *tinfo)
{

	GBT_VARKEY_R r = gbt_var_key_readable(node);
	int32		i = 0;
	int32		l = 0;
	int32		t1len = VARSIZE(r.lower) - VARHDRSZ;
	int32		t2len = VARSIZE(r.upper) - VARHDRSZ;
	int32		ml = Min(t1len, t2len);

	char	   *p1 = VARDATA(r.lower);
	char	   *p2 = VARDATA(r.upper);

	if (ml == 0)
		return 0;

	while (i < ml)
	{
		if (tinfo->eml > 1 && l == 0)
		{

			if ((l = pg_mblen(p1)) != pg_mblen(p2))
			{
				return i;
			}
		}
		if (*p1 != *p2)
		{
			if (tinfo->eml > 1)
			{
				return (i - l + 1);
			}
			else
			{
				return i;
			}
		}

		p1++;
		p2++;
		l--;
		i++;
	}
	return (ml);				/* lower == upper */
}


/*
 * returns true, if query matches prefix ( common prefix )
*/
static bool
gbt_bytea_pf_match(const bytea *pf, const bytea *query, const gbtree_vinfo *tinfo)
{

	bool		out = FALSE;
	int32		k = 0;
	int32		qlen = VARSIZE(query) - VARHDRSZ;
	int32		nlen = VARSIZE(pf) - VARHDRSZ;

	if (nlen <= qlen)
	{
		char	   *q = VARDATA(query);
		char	   *n = VARDATA(pf);

		if (tinfo->eml > 1)
		{
			out = (varstr_cmp(q, nlen, n, nlen) == 0);
		}
		else
		{
			out = TRUE;
			for (k = 0; k < nlen; k++)
			{
				if (*n != *q)
				{
					out = FALSE;
					break;
				}
				if (k < (nlen - 1))
				{
					q++;
					n++;
				}
			}
		}
	}

	return out;
}


/*
 * returns true, if query matches node using common prefix
*/

static bool
gbt_var_node_pf_match(const GBT_VARKEY_R *node, const bytea *query, const gbtree_vinfo *tinfo)
{

	return (tinfo->trnc && (
							gbt_bytea_pf_match(node->lower, query, tinfo) ||
							gbt_bytea_pf_match(node->upper, query, tinfo)
							));

}


/*
*  truncates / compresses the node key
*  cpf_length .. common prefix length
*/
static GBT_VARKEY *
gbt_var_node_truncate(const GBT_VARKEY *node, int32 cpf_length, const gbtree_vinfo *tinfo)
{
	GBT_VARKEY *out = NULL;
	GBT_VARKEY_R r = gbt_var_key_readable(node);
	int32		len1 = VARSIZE(r.lower) - VARHDRSZ;
	int32		len2 = VARSIZE(r.upper) - VARHDRSZ;
	int32		si;
	char	   *out2;

	len1 = Min(len1, (cpf_length + 1));
	len2 = Min(len2, (cpf_length + 1));

	si = 2 * VARHDRSZ + INTALIGN(len1 + VARHDRSZ) + len2;
	out = (GBT_VARKEY *) palloc(si);
	SET_VARSIZE(out, si);

	memcpy(VARDATA(out), r.lower, len1 + VARHDRSZ);
	SET_VARSIZE(VARDATA(out), len1 + VARHDRSZ);

	out2 = VARDATA(out) + INTALIGN(len1 + VARHDRSZ);
	memcpy(out2, r.upper, len2 + VARHDRSZ);
	SET_VARSIZE(out2, len2 + VARHDRSZ);

	return out;
}



void
gbt_var_bin_union(Datum *u, GBT_VARKEY *e, const gbtree_vinfo *tinfo)
{

	GBT_VARKEY *nk = NULL;
	GBT_VARKEY *tmp = NULL;
	GBT_VARKEY_R nr;
	GBT_VARKEY_R eo = gbt_var_key_readable(e);

	if (eo.lower == eo.upper)	/* leaf */
	{
		tmp = gbt_var_leaf2node(e, tinfo);
		if (tmp != e)
			eo = gbt_var_key_readable(tmp);
	}

	if (DatumGetPointer(*u))
	{

		GBT_VARKEY_R ro = gbt_var_key_readable((GBT_VARKEY *) DatumGetPointer(*u));

		if ((*tinfo->f_cmp) ((bytea *) ro.lower, (bytea *) eo.lower) > 0)
		{
			nr.lower = eo.lower;
			nr.upper = ro.upper;
			nk = gbt_var_key_copy(&nr, TRUE);
		}

		if ((*tinfo->f_cmp) ((bytea *) ro.upper, (bytea *) eo.upper) < 0)
		{
			nr.upper = eo.upper;
			nr.lower = ro.lower;
			nk = gbt_var_key_copy(&nr, TRUE);
		}

		if (nk)
			*u = PointerGetDatum(nk);
	}
	else
	{
		nr.lower = eo.lower;
		nr.upper = eo.upper;
		*u = PointerGetDatum(gbt_var_key_copy(&nr, TRUE));
	}
}



GISTENTRY *
gbt_var_compress(GISTENTRY *entry, const gbtree_vinfo *tinfo)
{

	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		GBT_VARKEY *r = NULL;
		bytea	   *leaf = (bytea *) DatumGetPointer(PG_DETOAST_DATUM(entry->key));
		GBT_VARKEY_R u;

		u.lower = u.upper = leaf;
		r = gbt_var_key_copy(&u, FALSE);

		retval = palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, TRUE);
	}
	else
		retval = entry;

	return (retval);
}



GBT_VARKEY *
gbt_var_union(const GistEntryVector *entryvec, int32 *size, const gbtree_vinfo *tinfo)
{

	int			i = 0,
				numranges = entryvec->n;
	GBT_VARKEY *cur;
	Datum		out;
	GBT_VARKEY_R rk;

	*size = sizeof(GBT_VARKEY);

	cur = (GBT_VARKEY *) DatumGetPointer(entryvec->vector[0].key);
	rk = gbt_var_key_readable(cur);
	out = PointerGetDatum(gbt_var_key_copy(&rk, TRUE));

	for (i = 1; i < numranges; i++)
	{
		cur = (GBT_VARKEY *) DatumGetPointer(entryvec->vector[i].key);
		gbt_var_bin_union(&out, cur, tinfo);
	}


	/* Truncate (=compress) key */
	if (tinfo->trnc)
	{
		int32		plen;
		GBT_VARKEY *trc = NULL;

		plen = gbt_var_node_cp_len((GBT_VARKEY *) DatumGetPointer(out), tinfo);
		trc = gbt_var_node_truncate((GBT_VARKEY *) DatumGetPointer(out), plen + 1, tinfo);

		out = PointerGetDatum(trc);
	}

	return ((GBT_VARKEY *) DatumGetPointer(out));
}


bool
gbt_var_same(bool *result, const Datum d1, const Datum d2, const gbtree_vinfo *tinfo)
{

	GBT_VARKEY *t1 = (GBT_VARKEY *) DatumGetPointer(d1);
	GBT_VARKEY *t2 = (GBT_VARKEY *) DatumGetPointer(d2);
	GBT_VARKEY_R r1,
				r2;

	r1 = gbt_var_key_readable(t1);
	r2 = gbt_var_key_readable(t2);

	if (t1 && t2)
	{
		*result = (((*tinfo->f_cmp) ((bytea *) r1.lower, (bytea *) r2.lower) == 0
					&& (*tinfo->f_cmp) ((bytea *) r1.upper, (bytea *) r2.upper) == 0) ? TRUE : FALSE);
	}
	else
		*result = (t1 == NULL && t2 == NULL) ? TRUE : FALSE;

	PG_RETURN_POINTER(result);
}



float *
gbt_var_penalty(float *res, const GISTENTRY *o, const GISTENTRY *n, const gbtree_vinfo *tinfo)
{

	GBT_VARKEY *orge = (GBT_VARKEY *) DatumGetPointer(o->key);
	GBT_VARKEY *newe = (GBT_VARKEY *) DatumGetPointer(n->key);
	GBT_VARKEY_R ok,
				nk;
	GBT_VARKEY *tmp = NULL;

	*res = 0.0;

	nk = gbt_var_key_readable(newe);
	if (nk.lower == nk.upper)	/* leaf */
	{
		tmp = gbt_var_leaf2node(newe, tinfo);
		if (tmp != newe)
			nk = gbt_var_key_readable(tmp);
	}
	ok = gbt_var_key_readable(orge);

	if ((VARSIZE(ok.lower) - VARHDRSZ) == 0 && (VARSIZE(ok.upper) - VARHDRSZ) == 0)
		*res = 0.0;
	else if (!(
			   (
				((*tinfo->f_cmp) (nk.lower, ok.lower) >= 0 || gbt_bytea_pf_match(ok.lower, nk.lower, tinfo)) &&
				((*tinfo->f_cmp) (nk.upper, ok.upper) <= 0 || gbt_bytea_pf_match(ok.upper, nk.upper, tinfo))
				)
			   ))
	{
		Datum		d = PointerGetDatum(0);
		double		dres = 0.0;
		int32		ol,
					ul;

		gbt_var_bin_union(&d, orge, tinfo);
		ol = gbt_var_node_cp_len((GBT_VARKEY *) DatumGetPointer(d), tinfo);
		gbt_var_bin_union(&d, newe, tinfo);
		ul = gbt_var_node_cp_len((GBT_VARKEY *) DatumGetPointer(d), tinfo);

		if (ul < ol)
		{
			dres = (ol - ul);	/* lost of common prefix len */
		}
		else
		{
			GBT_VARKEY_R uk = gbt_var_key_readable((GBT_VARKEY *) DatumGetPointer(d));

			char		tmp[4];

			tmp[0] = ((VARSIZE(ok.lower) - VARHDRSZ) == ul) ? (CHAR_MIN) : (VARDATA(ok.lower)[ul]);
			tmp[1] = ((VARSIZE(uk.lower) - VARHDRSZ) == ul) ? (CHAR_MIN) : (VARDATA(uk.lower)[ul]);
			tmp[2] = ((VARSIZE(ok.upper) - VARHDRSZ) == ul) ? (CHAR_MIN) : (VARDATA(ok.upper)[ul]);
			tmp[3] = ((VARSIZE(uk.upper) - VARHDRSZ) == ul) ? (CHAR_MIN) : (VARDATA(uk.upper)[ul]);
			dres = (tmp[0] - tmp[1]) +
				(tmp[3] - tmp[2]);
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
	const gbtree_vinfo *tinfo = (const gbtree_vinfo *) arg;

	return (*tinfo->f_cmp) (ar.lower, br.lower);
}

GIST_SPLITVEC *
gbt_var_picksplit(const GistEntryVector *entryvec, GIST_SPLITVEC *v, const gbtree_vinfo *tinfo)
{
	OffsetNumber i,
				maxoff = entryvec->n - 1;
	Vsrt	   *arr;
	int			svcntr = 0,
				nbytes;
	char	   *cur;
	GBT_VARKEY **sv = NULL;

	arr = (Vsrt *) palloc((maxoff + 1) * sizeof(Vsrt));
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);
	v->spl_ldatum = PointerGetDatum(0);
	v->spl_rdatum = PointerGetDatum(0);
	v->spl_nleft = 0;
	v->spl_nright = 0;

	sv = palloc(sizeof(bytea *) * (maxoff + 1));

	/* Sort entries */

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		GBT_VARKEY_R ro;

		cur = (char *) DatumGetPointer(entryvec->vector[i].key);
		ro = gbt_var_key_readable((GBT_VARKEY *) cur);
		if (ro.lower == ro.upper)		/* leaf */
		{
			sv[svcntr] = gbt_var_leaf2node((GBT_VARKEY *) cur, tinfo);
			arr[i].t = sv[svcntr];
			if (sv[svcntr] != (GBT_VARKEY *) cur)
				svcntr++;
		}
		else
			arr[i].t = (GBT_VARKEY *) cur;
		arr[i].i = i;
	}

	/* sort */
	qsort_arg((void *) &arr[FirstOffsetNumber],
			  maxoff - FirstOffsetNumber + 1,
			  sizeof(Vsrt),
			  gbt_vsrt_cmp,
			  (void *) tinfo);

	/* We do simply create two parts */

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		if (i <= (maxoff - FirstOffsetNumber + 1) / 2)
		{
			gbt_var_bin_union(&v->spl_ldatum, arr[i].t, tinfo);
			v->spl_left[v->spl_nleft] = arr[i].i;
			v->spl_nleft++;
		}
		else
		{
			gbt_var_bin_union(&v->spl_rdatum, arr[i].t, tinfo);
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
gbt_var_consistent(
				   GBT_VARKEY_R *key,
				   const void *query,
				   const StrategyNumber *strategy,
				   bool is_leaf,
				   const gbtree_vinfo *tinfo
)
{
	bool		retval = FALSE;

	switch (*strategy)
	{
		case BTLessEqualStrategyNumber:
			if (is_leaf)
				retval = (*tinfo->f_ge) (query, (void *) key->lower);
			else
				retval = (*tinfo->f_cmp) ((bytea *) query, key->lower) >= 0
					|| gbt_var_node_pf_match(key, query, tinfo);
			break;
		case BTLessStrategyNumber:
			if (is_leaf)
				retval = (*tinfo->f_gt) (query, (void *) key->lower);
			else
				retval = (*tinfo->f_cmp) ((bytea *) query, key->lower) >= 0
					|| gbt_var_node_pf_match(key, query, tinfo);
			break;
		case BTEqualStrategyNumber:
			if (is_leaf)
				retval = (*tinfo->f_eq) (query, (void *) key->lower);
			else
				retval = (
						  (
						(*tinfo->f_cmp) (key->lower, (bytea *) query) <= 0 &&
				  (*tinfo->f_cmp) ((bytea *) query, (void *) key->upper) <= 0
						   ) || gbt_var_node_pf_match(key, query, tinfo)
					);
			break;
		case BTGreaterStrategyNumber:
			if (is_leaf)
				retval = (*tinfo->f_lt) (query, (void *) key->upper);
			else
				retval = (*tinfo->f_cmp) ((bytea *) query, key->upper) <= 0
					|| gbt_var_node_pf_match(key, query, tinfo);
			break;
		case BTGreaterEqualStrategyNumber:
			if (is_leaf)
				retval = (*tinfo->f_le) (query, (void *) key->upper);
			else
				retval = (*tinfo->f_cmp) ((bytea *) query, key->upper) <= 0
					|| gbt_var_node_pf_match(key, query, tinfo);
			break;
		default:
			retval = FALSE;
	}

	return (retval);
}
