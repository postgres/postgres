/*
 * contrib/hstore/hstore_op.c
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "hstore.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

/* old names for C functions */
HSTORE_POLLUTE(hstore_fetchval, fetchval);
HSTORE_POLLUTE(hstore_exists, exists);
HSTORE_POLLUTE(hstore_defined, defined);
HSTORE_POLLUTE(hstore_delete, delete);
HSTORE_POLLUTE(hstore_concat, hs_concat);
HSTORE_POLLUTE(hstore_contains, hs_contains);
HSTORE_POLLUTE(hstore_contained, hs_contained);
HSTORE_POLLUTE(hstore_akeys, akeys);
HSTORE_POLLUTE(hstore_avals, avals);
HSTORE_POLLUTE(hstore_skeys, skeys);
HSTORE_POLLUTE(hstore_svals, svals);
HSTORE_POLLUTE(hstore_each, each);


/*
 * We're often finding a sequence of keys in ascending order. The
 * "lowbound" parameter is used to cache lower bounds of searches
 * between calls, based on this assumption. Pass NULL for it for
 * one-off or unordered searches.
 */
int
hstoreFindKey(HStore *hs, int *lowbound, char *key, int keylen)
{
	HEntry	   *entries = ARRPTR(hs);
	int			stopLow = lowbound ? *lowbound : 0;
	int			stopHigh = HS_COUNT(hs);
	int			stopMiddle;
	char	   *base = STRPTR(hs);

	while (stopLow < stopHigh)
	{
		int			difference;

		stopMiddle = stopLow + (stopHigh - stopLow) / 2;

		if (HSTORE_KEYLEN(entries, stopMiddle) == keylen)
			difference = memcmp(HSTORE_KEY(entries, base, stopMiddle), key, keylen);
		else
			difference = (HSTORE_KEYLEN(entries, stopMiddle) > keylen) ? 1 : -1;

		if (difference == 0)
		{
			if (lowbound)
				*lowbound = stopMiddle + 1;
			return stopMiddle;
		}
		else if (difference < 0)
			stopLow = stopMiddle + 1;
		else
			stopHigh = stopMiddle;
	}

	if (lowbound)
		*lowbound = stopLow;
	return -1;
}

Pairs *
hstoreArrayToPairs(ArrayType *a, int *npairs)
{
	Datum	   *key_datums;
	bool	   *key_nulls;
	int			key_count;
	Pairs	   *key_pairs;
	int			bufsiz;
	int			i,
				j;

	deconstruct_array(a,
					  TEXTOID, -1, false, TYPALIGN_INT,
					  &key_datums, &key_nulls, &key_count);

	if (key_count == 0)
	{
		*npairs = 0;
		return NULL;
	}

	/*
	 * A text array uses at least eight bytes per element, so any overflow in
	 * "key_count * sizeof(Pairs)" is small enough for palloc() to catch.
	 * However, credible improvements to the array format could invalidate
	 * that assumption.  Therefore, use an explicit check rather than relying
	 * on palloc() to complain.
	 */
	if (key_count > MaxAllocSize / sizeof(Pairs))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of pairs (%d) exceeds the maximum allowed (%d)",
						key_count, (int) (MaxAllocSize / sizeof(Pairs)))));

	key_pairs = palloc(sizeof(Pairs) * key_count);

	for (i = 0, j = 0; i < key_count; i++)
	{
		if (!key_nulls[i])
		{
			key_pairs[j].key = VARDATA(key_datums[i]);
			key_pairs[j].keylen = VARSIZE(key_datums[i]) - VARHDRSZ;
			key_pairs[j].val = NULL;
			key_pairs[j].vallen = 0;
			key_pairs[j].needfree = 0;
			key_pairs[j].isnull = 1;
			j++;
		}
	}

	*npairs = hstoreUniquePairs(key_pairs, j, &bufsiz);

	return key_pairs;
}


PG_FUNCTION_INFO_V1(hstore_fetchval);
Datum
hstore_fetchval(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	HEntry	   *entries = ARRPTR(hs);
	text	   *out;
	int			idx = hstoreFindKey(hs, NULL,
									VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key));

	if (idx < 0 || HSTORE_VALISNULL(entries, idx))
		PG_RETURN_NULL();

	out = cstring_to_text_with_len(HSTORE_VAL(entries, STRPTR(hs), idx),
								   HSTORE_VALLEN(entries, idx));

	PG_RETURN_TEXT_P(out);
}


PG_FUNCTION_INFO_V1(hstore_exists);
Datum
hstore_exists(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	int			idx = hstoreFindKey(hs, NULL,
									VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key));

	PG_RETURN_BOOL(idx >= 0);
}


PG_FUNCTION_INFO_V1(hstore_exists_any);
Datum
hstore_exists_any(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	ArrayType  *keys = PG_GETARG_ARRAYTYPE_P(1);
	int			nkeys;
	Pairs	   *key_pairs = hstoreArrayToPairs(keys, &nkeys);
	int			i;
	int			lowbound = 0;
	bool		res = false;

	/*
	 * we exploit the fact that the pairs list is already sorted into strictly
	 * increasing order to narrow the hstoreFindKey search; each search can
	 * start one entry past the previous "found" entry, or at the lower bound
	 * of the last search.
	 */
	for (i = 0; i < nkeys; i++)
	{
		int			idx = hstoreFindKey(hs, &lowbound,
										key_pairs[i].key, key_pairs[i].keylen);

		if (idx >= 0)
		{
			res = true;
			break;
		}
	}

	PG_RETURN_BOOL(res);
}


PG_FUNCTION_INFO_V1(hstore_exists_all);
Datum
hstore_exists_all(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	ArrayType  *keys = PG_GETARG_ARRAYTYPE_P(1);
	int			nkeys;
	Pairs	   *key_pairs = hstoreArrayToPairs(keys, &nkeys);
	int			i;
	int			lowbound = 0;
	bool		res = true;

	/*
	 * we exploit the fact that the pairs list is already sorted into strictly
	 * increasing order to narrow the hstoreFindKey search; each search can
	 * start one entry past the previous "found" entry, or at the lower bound
	 * of the last search.
	 */
	for (i = 0; i < nkeys; i++)
	{
		int			idx = hstoreFindKey(hs, &lowbound,
										key_pairs[i].key, key_pairs[i].keylen);

		if (idx < 0)
		{
			res = false;
			break;
		}
	}

	PG_RETURN_BOOL(res);
}


PG_FUNCTION_INFO_V1(hstore_defined);
Datum
hstore_defined(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	HEntry	   *entries = ARRPTR(hs);
	int			idx = hstoreFindKey(hs, NULL,
									VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key));
	bool		res = (idx >= 0 && !HSTORE_VALISNULL(entries, idx));

	PG_RETURN_BOOL(res);
}


PG_FUNCTION_INFO_V1(hstore_delete);
Datum
hstore_delete(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	char	   *keyptr = VARDATA_ANY(key);
	int			keylen = VARSIZE_ANY_EXHDR(key);
	HStore	   *out = palloc(VARSIZE(hs));
	char	   *bufs,
			   *bufd,
			   *ptrd;
	HEntry	   *es,
			   *ed;
	int			i;
	int			count = HS_COUNT(hs);
	int			outcount = 0;

	SET_VARSIZE(out, VARSIZE(hs));
	HS_SETCOUNT(out, count);	/* temporary! */

	bufs = STRPTR(hs);
	es = ARRPTR(hs);
	bufd = ptrd = STRPTR(out);
	ed = ARRPTR(out);

	for (i = 0; i < count; ++i)
	{
		int			len = HSTORE_KEYLEN(es, i);
		char	   *ptrs = HSTORE_KEY(es, bufs, i);

		if (!(len == keylen && memcmp(ptrs, keyptr, keylen) == 0))
		{
			int			vallen = HSTORE_VALLEN(es, i);

			HS_COPYITEM(ed, bufd, ptrd, ptrs, len, vallen,
						HSTORE_VALISNULL(es, i));
			++outcount;
		}
	}

	HS_FINALIZE(out, outcount, bufd, ptrd);

	PG_RETURN_POINTER(out);
}


PG_FUNCTION_INFO_V1(hstore_delete_array);
Datum
hstore_delete_array(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	HStore	   *out = palloc(VARSIZE(hs));
	int			hs_count = HS_COUNT(hs);
	char	   *ps,
			   *bufd,
			   *pd;
	HEntry	   *es,
			   *ed;
	int			i,
				j;
	int			outcount = 0;
	ArrayType  *key_array = PG_GETARG_ARRAYTYPE_P(1);
	int			nkeys;
	Pairs	   *key_pairs = hstoreArrayToPairs(key_array, &nkeys);

	SET_VARSIZE(out, VARSIZE(hs));
	HS_SETCOUNT(out, hs_count); /* temporary! */

	ps = STRPTR(hs);
	es = ARRPTR(hs);
	bufd = pd = STRPTR(out);
	ed = ARRPTR(out);

	if (nkeys == 0)
	{
		/* return a copy of the input, unchanged */
		memcpy(out, hs, VARSIZE(hs));
		HS_FIXSIZE(out, hs_count);
		HS_SETCOUNT(out, hs_count);
		PG_RETURN_POINTER(out);
	}

	/*
	 * this is in effect a merge between hs and key_pairs, both of which are
	 * already sorted by (keylen,key); we take keys from hs only
	 */

	for (i = j = 0; i < hs_count;)
	{
		int			difference;

		if (j >= nkeys)
			difference = -1;
		else
		{
			int			skeylen = HSTORE_KEYLEN(es, i);

			if (skeylen == key_pairs[j].keylen)
				difference = memcmp(HSTORE_KEY(es, ps, i),
									key_pairs[j].key,
									key_pairs[j].keylen);
			else
				difference = (skeylen > key_pairs[j].keylen) ? 1 : -1;
		}

		if (difference > 0)
			++j;
		else if (difference == 0)
			++i, ++j;
		else
		{
			HS_COPYITEM(ed, bufd, pd,
						HSTORE_KEY(es, ps, i), HSTORE_KEYLEN(es, i),
						HSTORE_VALLEN(es, i), HSTORE_VALISNULL(es, i));
			++outcount;
			++i;
		}
	}

	HS_FINALIZE(out, outcount, bufd, pd);

	PG_RETURN_POINTER(out);
}


PG_FUNCTION_INFO_V1(hstore_delete_hstore);
Datum
hstore_delete_hstore(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	HStore	   *hs2 = PG_GETARG_HSTORE_P(1);
	HStore	   *out = palloc(VARSIZE(hs));
	int			hs_count = HS_COUNT(hs);
	int			hs2_count = HS_COUNT(hs2);
	char	   *ps,
			   *ps2,
			   *bufd,
			   *pd;
	HEntry	   *es,
			   *es2,
			   *ed;
	int			i,
				j;
	int			outcount = 0;

	SET_VARSIZE(out, VARSIZE(hs));
	HS_SETCOUNT(out, hs_count); /* temporary! */

	ps = STRPTR(hs);
	es = ARRPTR(hs);
	ps2 = STRPTR(hs2);
	es2 = ARRPTR(hs2);
	bufd = pd = STRPTR(out);
	ed = ARRPTR(out);

	if (hs2_count == 0)
	{
		/* return a copy of the input, unchanged */
		memcpy(out, hs, VARSIZE(hs));
		HS_FIXSIZE(out, hs_count);
		HS_SETCOUNT(out, hs_count);
		PG_RETURN_POINTER(out);
	}

	/*
	 * this is in effect a merge between hs and hs2, both of which are already
	 * sorted by (keylen,key); we take keys from hs only; for equal keys, we
	 * take the value from hs unless the values are equal
	 */

	for (i = j = 0; i < hs_count;)
	{
		int			difference;

		if (j >= hs2_count)
			difference = -1;
		else
		{
			int			skeylen = HSTORE_KEYLEN(es, i);
			int			s2keylen = HSTORE_KEYLEN(es2, j);

			if (skeylen == s2keylen)
				difference = memcmp(HSTORE_KEY(es, ps, i),
									HSTORE_KEY(es2, ps2, j),
									skeylen);
			else
				difference = (skeylen > s2keylen) ? 1 : -1;
		}

		if (difference > 0)
			++j;
		else if (difference == 0)
		{
			int			svallen = HSTORE_VALLEN(es, i);
			int			snullval = HSTORE_VALISNULL(es, i);

			if (snullval != HSTORE_VALISNULL(es2, j) ||
				(!snullval && (svallen != HSTORE_VALLEN(es2, j) ||
							   memcmp(HSTORE_VAL(es, ps, i),
									  HSTORE_VAL(es2, ps2, j),
									  svallen) != 0)))
			{
				HS_COPYITEM(ed, bufd, pd,
							HSTORE_KEY(es, ps, i), HSTORE_KEYLEN(es, i),
							svallen, snullval);
				++outcount;
			}
			++i, ++j;
		}
		else
		{
			HS_COPYITEM(ed, bufd, pd,
						HSTORE_KEY(es, ps, i), HSTORE_KEYLEN(es, i),
						HSTORE_VALLEN(es, i), HSTORE_VALISNULL(es, i));
			++outcount;
			++i;
		}
	}

	HS_FINALIZE(out, outcount, bufd, pd);

	PG_RETURN_POINTER(out);
}


PG_FUNCTION_INFO_V1(hstore_concat);
Datum
hstore_concat(PG_FUNCTION_ARGS)
{
	HStore	   *s1 = PG_GETARG_HSTORE_P(0);
	HStore	   *s2 = PG_GETARG_HSTORE_P(1);
	HStore	   *out = palloc(VARSIZE(s1) + VARSIZE(s2));
	char	   *ps1,
			   *ps2,
			   *bufd,
			   *pd;
	HEntry	   *es1,
			   *es2,
			   *ed;
	int			s1idx;
	int			s2idx;
	int			s1count = HS_COUNT(s1);
	int			s2count = HS_COUNT(s2);
	int			outcount = 0;

	SET_VARSIZE(out, VARSIZE(s1) + VARSIZE(s2) - HSHRDSIZE);
	HS_SETCOUNT(out, s1count + s2count);

	if (s1count == 0)
	{
		/* return a copy of the input, unchanged */
		memcpy(out, s2, VARSIZE(s2));
		HS_FIXSIZE(out, s2count);
		HS_SETCOUNT(out, s2count);
		PG_RETURN_POINTER(out);
	}

	if (s2count == 0)
	{
		/* return a copy of the input, unchanged */
		memcpy(out, s1, VARSIZE(s1));
		HS_FIXSIZE(out, s1count);
		HS_SETCOUNT(out, s1count);
		PG_RETURN_POINTER(out);
	}

	ps1 = STRPTR(s1);
	ps2 = STRPTR(s2);
	bufd = pd = STRPTR(out);
	es1 = ARRPTR(s1);
	es2 = ARRPTR(s2);
	ed = ARRPTR(out);

	/*
	 * this is in effect a merge between s1 and s2, both of which are already
	 * sorted by (keylen,key); we take s2 for equal keys
	 */

	for (s1idx = s2idx = 0; s1idx < s1count || s2idx < s2count; ++outcount)
	{
		int			difference;

		if (s1idx >= s1count)
			difference = 1;
		else if (s2idx >= s2count)
			difference = -1;
		else
		{
			int			s1keylen = HSTORE_KEYLEN(es1, s1idx);
			int			s2keylen = HSTORE_KEYLEN(es2, s2idx);

			if (s1keylen == s2keylen)
				difference = memcmp(HSTORE_KEY(es1, ps1, s1idx),
									HSTORE_KEY(es2, ps2, s2idx),
									s1keylen);
			else
				difference = (s1keylen > s2keylen) ? 1 : -1;
		}

		if (difference >= 0)
		{
			HS_COPYITEM(ed, bufd, pd,
						HSTORE_KEY(es2, ps2, s2idx), HSTORE_KEYLEN(es2, s2idx),
						HSTORE_VALLEN(es2, s2idx), HSTORE_VALISNULL(es2, s2idx));
			++s2idx;
			if (difference == 0)
				++s1idx;
		}
		else
		{
			HS_COPYITEM(ed, bufd, pd,
						HSTORE_KEY(es1, ps1, s1idx), HSTORE_KEYLEN(es1, s1idx),
						HSTORE_VALLEN(es1, s1idx), HSTORE_VALISNULL(es1, s1idx));
			++s1idx;
		}
	}

	HS_FINALIZE(out, outcount, bufd, pd);

	PG_RETURN_POINTER(out);
}


PG_FUNCTION_INFO_V1(hstore_slice_to_array);
Datum
hstore_slice_to_array(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	HEntry	   *entries = ARRPTR(hs);
	char	   *ptr = STRPTR(hs);
	ArrayType  *key_array = PG_GETARG_ARRAYTYPE_P(1);
	ArrayType  *aout;
	Datum	   *key_datums;
	bool	   *key_nulls;
	Datum	   *out_datums;
	bool	   *out_nulls;
	int			key_count;
	int			i;

	deconstruct_array(key_array,
					  TEXTOID, -1, false, TYPALIGN_INT,
					  &key_datums, &key_nulls, &key_count);

	if (key_count == 0)
	{
		aout = construct_empty_array(TEXTOID);
		PG_RETURN_POINTER(aout);
	}

	out_datums = palloc(sizeof(Datum) * key_count);
	out_nulls = palloc(sizeof(bool) * key_count);

	for (i = 0; i < key_count; ++i)
	{
		text	   *key = (text *) DatumGetPointer(key_datums[i]);
		int			idx;

		if (key_nulls[i])
			idx = -1;
		else
			idx = hstoreFindKey(hs, NULL, VARDATA(key), VARSIZE(key) - VARHDRSZ);

		if (idx < 0 || HSTORE_VALISNULL(entries, idx))
		{
			out_nulls[i] = true;
			out_datums[i] = (Datum) 0;
		}
		else
		{
			out_datums[i] =
				PointerGetDatum(cstring_to_text_with_len(HSTORE_VAL(entries, ptr, idx),
														 HSTORE_VALLEN(entries, idx)));
			out_nulls[i] = false;
		}
	}

	aout = construct_md_array(out_datums, out_nulls,
							  ARR_NDIM(key_array),
							  ARR_DIMS(key_array),
							  ARR_LBOUND(key_array),
							  TEXTOID, -1, false, TYPALIGN_INT);

	PG_RETURN_POINTER(aout);
}


PG_FUNCTION_INFO_V1(hstore_slice_to_hstore);
Datum
hstore_slice_to_hstore(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	HEntry	   *entries = ARRPTR(hs);
	char	   *ptr = STRPTR(hs);
	ArrayType  *key_array = PG_GETARG_ARRAYTYPE_P(1);
	HStore	   *out;
	int			nkeys;
	Pairs	   *key_pairs = hstoreArrayToPairs(key_array, &nkeys);
	Pairs	   *out_pairs;
	int			bufsiz;
	int			lastidx = 0;
	int			i;
	int			out_count = 0;

	if (nkeys == 0)
	{
		out = hstorePairs(NULL, 0, 0);
		PG_RETURN_POINTER(out);
	}

	/* hstoreArrayToPairs() checked overflow */
	out_pairs = palloc(sizeof(Pairs) * nkeys);
	bufsiz = 0;

	/*
	 * we exploit the fact that the pairs list is already sorted into strictly
	 * increasing order to narrow the hstoreFindKey search; each search can
	 * start one entry past the previous "found" entry, or at the lower bound
	 * of the last search.
	 */

	for (i = 0; i < nkeys; ++i)
	{
		int			idx = hstoreFindKey(hs, &lastidx,
										key_pairs[i].key, key_pairs[i].keylen);

		if (idx >= 0)
		{
			out_pairs[out_count].key = key_pairs[i].key;
			bufsiz += (out_pairs[out_count].keylen = key_pairs[i].keylen);
			out_pairs[out_count].val = HSTORE_VAL(entries, ptr, idx);
			bufsiz += (out_pairs[out_count].vallen = HSTORE_VALLEN(entries, idx));
			out_pairs[out_count].isnull = HSTORE_VALISNULL(entries, idx);
			out_pairs[out_count].needfree = false;
			++out_count;
		}
	}

	/*
	 * we don't use hstoreUniquePairs here because we know that the pairs list
	 * is already sorted and uniq'ed.
	 */

	out = hstorePairs(out_pairs, out_count, bufsiz);

	PG_RETURN_POINTER(out);
}


PG_FUNCTION_INFO_V1(hstore_akeys);
Datum
hstore_akeys(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	Datum	   *d;
	ArrayType  *a;
	HEntry	   *entries = ARRPTR(hs);
	char	   *base = STRPTR(hs);
	int			count = HS_COUNT(hs);
	int			i;

	if (count == 0)
	{
		a = construct_empty_array(TEXTOID);
		PG_RETURN_POINTER(a);
	}

	d = (Datum *) palloc(sizeof(Datum) * count);

	for (i = 0; i < count; ++i)
	{
		text	   *t = cstring_to_text_with_len(HSTORE_KEY(entries, base, i),
												 HSTORE_KEYLEN(entries, i));

		d[i] = PointerGetDatum(t);
	}

	a = construct_array(d, count,
						TEXTOID, -1, false, TYPALIGN_INT);

	PG_RETURN_POINTER(a);
}


PG_FUNCTION_INFO_V1(hstore_avals);
Datum
hstore_avals(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	Datum	   *d;
	bool	   *nulls;
	ArrayType  *a;
	HEntry	   *entries = ARRPTR(hs);
	char	   *base = STRPTR(hs);
	int			count = HS_COUNT(hs);
	int			lb = 1;
	int			i;

	if (count == 0)
	{
		a = construct_empty_array(TEXTOID);
		PG_RETURN_POINTER(a);
	}

	d = (Datum *) palloc(sizeof(Datum) * count);
	nulls = (bool *) palloc(sizeof(bool) * count);

	for (i = 0; i < count; ++i)
	{
		if (HSTORE_VALISNULL(entries, i))
		{
			d[i] = (Datum) 0;
			nulls[i] = true;
		}
		else
		{
			text	   *item = cstring_to_text_with_len(HSTORE_VAL(entries, base, i),
														HSTORE_VALLEN(entries, i));

			d[i] = PointerGetDatum(item);
			nulls[i] = false;
		}
	}

	a = construct_md_array(d, nulls, 1, &count, &lb,
						   TEXTOID, -1, false, TYPALIGN_INT);

	PG_RETURN_POINTER(a);
}


static ArrayType *
hstore_to_array_internal(HStore *hs, int ndims)
{
	HEntry	   *entries = ARRPTR(hs);
	char	   *base = STRPTR(hs);
	int			count = HS_COUNT(hs);
	int			out_size[2] = {0, 2};
	int			lb[2] = {1, 1};
	Datum	   *out_datums;
	bool	   *out_nulls;
	int			i;

	Assert(ndims < 3);

	if (count == 0 || ndims == 0)
		return construct_empty_array(TEXTOID);

	out_size[0] = count * 2 / ndims;
	out_datums = palloc(sizeof(Datum) * count * 2);
	out_nulls = palloc(sizeof(bool) * count * 2);

	for (i = 0; i < count; ++i)
	{
		text	   *key = cstring_to_text_with_len(HSTORE_KEY(entries, base, i),
												   HSTORE_KEYLEN(entries, i));

		out_datums[i * 2] = PointerGetDatum(key);
		out_nulls[i * 2] = false;

		if (HSTORE_VALISNULL(entries, i))
		{
			out_datums[i * 2 + 1] = (Datum) 0;
			out_nulls[i * 2 + 1] = true;
		}
		else
		{
			text	   *item = cstring_to_text_with_len(HSTORE_VAL(entries, base, i),
														HSTORE_VALLEN(entries, i));

			out_datums[i * 2 + 1] = PointerGetDatum(item);
			out_nulls[i * 2 + 1] = false;
		}
	}

	return construct_md_array(out_datums, out_nulls,
							  ndims, out_size, lb,
							  TEXTOID, -1, false, TYPALIGN_INT);
}

PG_FUNCTION_INFO_V1(hstore_to_array);
Datum
hstore_to_array(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	ArrayType  *out = hstore_to_array_internal(hs, 1);

	PG_RETURN_POINTER(out);
}

PG_FUNCTION_INFO_V1(hstore_to_matrix);
Datum
hstore_to_matrix(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	ArrayType  *out = hstore_to_array_internal(hs, 2);

	PG_RETURN_POINTER(out);
}

/*
 * Common initialization function for the various set-returning
 * funcs. fcinfo is only passed if the function is to return a
 * composite; it will be used to look up the return tupledesc.
 * we stash a copy of the hstore in the multi-call context in
 * case it was originally toasted. (At least I assume that's why;
 * there was no explanatory comment in the original code. --AG)
 */

static void
setup_firstcall(FuncCallContext *funcctx, HStore *hs,
				FunctionCallInfo fcinfo)
{
	MemoryContext oldcontext;
	HStore	   *st;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	st = (HStore *) palloc(VARSIZE(hs));
	memcpy(st, hs, VARSIZE(hs));

	funcctx->user_fctx = (void *) st;

	if (fcinfo)
	{
		TupleDesc	tupdesc;

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
	}

	MemoryContextSwitchTo(oldcontext);
}


PG_FUNCTION_INFO_V1(hstore_skeys);
Datum
hstore_skeys(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	HStore	   *hs;
	int			i;

	if (SRF_IS_FIRSTCALL())
	{
		hs = PG_GETARG_HSTORE_P(0);
		funcctx = SRF_FIRSTCALL_INIT();
		setup_firstcall(funcctx, hs, NULL);
	}

	funcctx = SRF_PERCALL_SETUP();
	hs = (HStore *) funcctx->user_fctx;
	i = funcctx->call_cntr;

	if (i < HS_COUNT(hs))
	{
		HEntry	   *entries = ARRPTR(hs);
		text	   *item;

		item = cstring_to_text_with_len(HSTORE_KEY(entries, STRPTR(hs), i),
										HSTORE_KEYLEN(entries, i));

		SRF_RETURN_NEXT(funcctx, PointerGetDatum(item));
	}

	SRF_RETURN_DONE(funcctx);
}


PG_FUNCTION_INFO_V1(hstore_svals);
Datum
hstore_svals(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	HStore	   *hs;
	int			i;

	if (SRF_IS_FIRSTCALL())
	{
		hs = PG_GETARG_HSTORE_P(0);
		funcctx = SRF_FIRSTCALL_INIT();
		setup_firstcall(funcctx, hs, NULL);
	}

	funcctx = SRF_PERCALL_SETUP();
	hs = (HStore *) funcctx->user_fctx;
	i = funcctx->call_cntr;

	if (i < HS_COUNT(hs))
	{
		HEntry	   *entries = ARRPTR(hs);

		if (HSTORE_VALISNULL(entries, i))
		{
			ReturnSetInfo *rsi;

			/* ugly ugly ugly. why no macro for this? */
			(funcctx)->call_cntr++;
			rsi = (ReturnSetInfo *) fcinfo->resultinfo;
			rsi->isDone = ExprMultipleResult;
			PG_RETURN_NULL();
		}
		else
		{
			text	   *item;

			item = cstring_to_text_with_len(HSTORE_VAL(entries, STRPTR(hs), i),
											HSTORE_VALLEN(entries, i));

			SRF_RETURN_NEXT(funcctx, PointerGetDatum(item));
		}
	}

	SRF_RETURN_DONE(funcctx);
}


PG_FUNCTION_INFO_V1(hstore_contains);
Datum
hstore_contains(PG_FUNCTION_ARGS)
{
	HStore	   *val = PG_GETARG_HSTORE_P(0);
	HStore	   *tmpl = PG_GETARG_HSTORE_P(1);
	bool		res = true;
	HEntry	   *te = ARRPTR(tmpl);
	char	   *tstr = STRPTR(tmpl);
	HEntry	   *ve = ARRPTR(val);
	char	   *vstr = STRPTR(val);
	int			tcount = HS_COUNT(tmpl);
	int			lastidx = 0;
	int			i;

	/*
	 * we exploit the fact that keys in "tmpl" are in strictly increasing
	 * order to narrow the hstoreFindKey search; each search can start one
	 * entry past the previous "found" entry, or at the lower bound of the
	 * search
	 */

	for (i = 0; res && i < tcount; ++i)
	{
		int			idx = hstoreFindKey(val, &lastidx,
										HSTORE_KEY(te, tstr, i),
										HSTORE_KEYLEN(te, i));

		if (idx >= 0)
		{
			bool		nullval = HSTORE_VALISNULL(te, i);
			int			vallen = HSTORE_VALLEN(te, i);

			if (nullval != HSTORE_VALISNULL(ve, idx) ||
				(!nullval && (vallen != HSTORE_VALLEN(ve, idx) ||
							  memcmp(HSTORE_VAL(te, tstr, i),
									 HSTORE_VAL(ve, vstr, idx),
									 vallen) != 0)))
				res = false;
		}
		else
			res = false;
	}

	PG_RETURN_BOOL(res);
}


PG_FUNCTION_INFO_V1(hstore_contained);
Datum
hstore_contained(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2(hstore_contains,
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(0)
										));
}


PG_FUNCTION_INFO_V1(hstore_each);
Datum
hstore_each(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	HStore	   *hs;
	int			i;

	if (SRF_IS_FIRSTCALL())
	{
		hs = PG_GETARG_HSTORE_P(0);
		funcctx = SRF_FIRSTCALL_INIT();
		setup_firstcall(funcctx, hs, fcinfo);
	}

	funcctx = SRF_PERCALL_SETUP();
	hs = (HStore *) funcctx->user_fctx;
	i = funcctx->call_cntr;

	if (i < HS_COUNT(hs))
	{
		HEntry	   *entries = ARRPTR(hs);
		char	   *ptr = STRPTR(hs);
		Datum		res,
					dvalues[2];
		bool		nulls[2] = {false, false};
		text	   *item;
		HeapTuple	tuple;

		item = cstring_to_text_with_len(HSTORE_KEY(entries, ptr, i),
										HSTORE_KEYLEN(entries, i));
		dvalues[0] = PointerGetDatum(item);

		if (HSTORE_VALISNULL(entries, i))
		{
			dvalues[1] = (Datum) 0;
			nulls[1] = true;
		}
		else
		{
			item = cstring_to_text_with_len(HSTORE_VAL(entries, ptr, i),
											HSTORE_VALLEN(entries, i));
			dvalues[1] = PointerGetDatum(item);
		}

		tuple = heap_form_tuple(funcctx->tuple_desc, dvalues, nulls);
		res = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, PointerGetDatum(res));
	}

	SRF_RETURN_DONE(funcctx);
}


/*
 * btree sort order for hstores isn't intended to be useful; we really only
 * care about equality versus non-equality.  we compare the entire string
 * buffer first, then the entry pos array.
 */

PG_FUNCTION_INFO_V1(hstore_cmp);
Datum
hstore_cmp(PG_FUNCTION_ARGS)
{
	HStore	   *hs1 = PG_GETARG_HSTORE_P(0);
	HStore	   *hs2 = PG_GETARG_HSTORE_P(1);
	int			hcount1 = HS_COUNT(hs1);
	int			hcount2 = HS_COUNT(hs2);
	int			res = 0;

	if (hcount1 == 0 || hcount2 == 0)
	{
		/*
		 * if either operand is empty, and the other is nonempty, the nonempty
		 * one is larger. If both are empty they are equal.
		 */
		if (hcount1 > 0)
			res = 1;
		else if (hcount2 > 0)
			res = -1;
	}
	else
	{
		/* here we know both operands are nonempty */
		char	   *str1 = STRPTR(hs1);
		char	   *str2 = STRPTR(hs2);
		HEntry	   *ent1 = ARRPTR(hs1);
		HEntry	   *ent2 = ARRPTR(hs2);
		size_t		len1 = HSE_ENDPOS(ent1[2 * hcount1 - 1]);
		size_t		len2 = HSE_ENDPOS(ent2[2 * hcount2 - 1]);

		res = memcmp(str1, str2, Min(len1, len2));

		if (res == 0)
		{
			if (len1 > len2)
				res = 1;
			else if (len1 < len2)
				res = -1;
			else if (hcount1 > hcount2)
				res = 1;
			else if (hcount2 > hcount1)
				res = -1;
			else
			{
				int			count = hcount1 * 2;
				int			i;

				for (i = 0; i < count; ++i)
					if (HSE_ENDPOS(ent1[i]) != HSE_ENDPOS(ent2[i]) ||
						HSE_ISNULL(ent1[i]) != HSE_ISNULL(ent2[i]))
						break;
				if (i < count)
				{
					if (HSE_ENDPOS(ent1[i]) < HSE_ENDPOS(ent2[i]))
						res = -1;
					else if (HSE_ENDPOS(ent1[i]) > HSE_ENDPOS(ent2[i]))
						res = 1;
					else if (HSE_ISNULL(ent1[i]))
						res = 1;
					else if (HSE_ISNULL(ent2[i]))
						res = -1;
				}
			}
		}
		else
		{
			res = (res > 0) ? 1 : -1;
		}
	}

	/*
	 * this is a btree support function; this is one of the few places where
	 * memory needs to be explicitly freed.
	 */
	PG_FREE_IF_COPY(hs1, 0);
	PG_FREE_IF_COPY(hs2, 1);
	PG_RETURN_INT32(res);
}


PG_FUNCTION_INFO_V1(hstore_eq);
Datum
hstore_eq(PG_FUNCTION_ARGS)
{
	int			res = DatumGetInt32(DirectFunctionCall2(hstore_cmp,
														PG_GETARG_DATUM(0),
														PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(res == 0);
}

PG_FUNCTION_INFO_V1(hstore_ne);
Datum
hstore_ne(PG_FUNCTION_ARGS)
{
	int			res = DatumGetInt32(DirectFunctionCall2(hstore_cmp,
														PG_GETARG_DATUM(0),
														PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(res != 0);
}

PG_FUNCTION_INFO_V1(hstore_gt);
Datum
hstore_gt(PG_FUNCTION_ARGS)
{
	int			res = DatumGetInt32(DirectFunctionCall2(hstore_cmp,
														PG_GETARG_DATUM(0),
														PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(res > 0);
}

PG_FUNCTION_INFO_V1(hstore_ge);
Datum
hstore_ge(PG_FUNCTION_ARGS)
{
	int			res = DatumGetInt32(DirectFunctionCall2(hstore_cmp,
														PG_GETARG_DATUM(0),
														PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(res >= 0);
}

PG_FUNCTION_INFO_V1(hstore_lt);
Datum
hstore_lt(PG_FUNCTION_ARGS)
{
	int			res = DatumGetInt32(DirectFunctionCall2(hstore_cmp,
														PG_GETARG_DATUM(0),
														PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(res < 0);
}

PG_FUNCTION_INFO_V1(hstore_le);
Datum
hstore_le(PG_FUNCTION_ARGS)
{
	int			res = DatumGetInt32(DirectFunctionCall2(hstore_cmp,
														PG_GETARG_DATUM(0),
														PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(res <= 0);
}


PG_FUNCTION_INFO_V1(hstore_hash);
Datum
hstore_hash(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	Datum		hval = hash_any((unsigned char *) VARDATA(hs),
								VARSIZE(hs) - VARHDRSZ);

	/*
	 * This (along with hstore_hash_extended) is the only place in the code
	 * that cares whether the overall varlena size exactly matches the true
	 * data size; this assertion should be maintained by all the other code,
	 * but we make it explicit here.
	 */
	Assert(VARSIZE(hs) ==
		   (HS_COUNT(hs) != 0 ?
			CALCDATASIZE(HS_COUNT(hs),
						 HSE_ENDPOS(ARRPTR(hs)[2 * HS_COUNT(hs) - 1])) :
			HSHRDSIZE));

	PG_FREE_IF_COPY(hs, 0);
	PG_RETURN_DATUM(hval);
}

PG_FUNCTION_INFO_V1(hstore_hash_extended);
Datum
hstore_hash_extended(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	uint64		seed = PG_GETARG_INT64(1);
	Datum		hval;

	hval = hash_any_extended((unsigned char *) VARDATA(hs),
							 VARSIZE(hs) - VARHDRSZ,
							 seed);

	/* See comment in hstore_hash */
	Assert(VARSIZE(hs) ==
		   (HS_COUNT(hs) != 0 ?
			CALCDATASIZE(HS_COUNT(hs),
						 HSE_ENDPOS(ARRPTR(hs)[2 * HS_COUNT(hs) - 1])) :
			HSHRDSIZE));

	PG_FREE_IF_COPY(hs, 0);
	PG_RETURN_DATUM(hval);
}
