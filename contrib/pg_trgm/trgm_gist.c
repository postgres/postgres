/*
 * contrib/pg_trgm/trgm_gist.c
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "access/stratnum.h"
#include "fmgr.h"
#include "port/pg_bitutils.h"
#include "trgm.h"
#include "varatt.h"

/* gist_trgm_ops opclass options */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			siglen;			/* signature length in bytes */
} TrgmGistOptions;

#define GET_SIGLEN()			(PG_HAS_OPCLASS_OPTIONS() ? \
								 ((TrgmGistOptions *) PG_GET_OPCLASS_OPTIONS())->siglen : \
								 SIGLEN_DEFAULT)

typedef struct
{
	/* most recent inputs to gtrgm_consistent */
	StrategyNumber strategy;
	text	   *query;
	/* extracted trigrams for query */
	TRGM	   *trigrams;
	/* if a regex operator, the extracted graph */
	TrgmPackedGraph *graph;

	/*
	 * The "query" and "trigrams" are stored in the same palloc block as this
	 * cache struct, at MAXALIGN'ed offsets.  The graph however isn't.
	 */
} gtrgm_consistent_cache;

#define GETENTRY(vec,pos) ((TRGM *) DatumGetPointer((vec)->vector[(pos)].key))


PG_FUNCTION_INFO_V1(gtrgm_in);
PG_FUNCTION_INFO_V1(gtrgm_out);
PG_FUNCTION_INFO_V1(gtrgm_compress);
PG_FUNCTION_INFO_V1(gtrgm_decompress);
PG_FUNCTION_INFO_V1(gtrgm_consistent);
PG_FUNCTION_INFO_V1(gtrgm_distance);
PG_FUNCTION_INFO_V1(gtrgm_union);
PG_FUNCTION_INFO_V1(gtrgm_same);
PG_FUNCTION_INFO_V1(gtrgm_penalty);
PG_FUNCTION_INFO_V1(gtrgm_picksplit);
PG_FUNCTION_INFO_V1(gtrgm_options);


Datum
gtrgm_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "gtrgm")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

Datum
gtrgm_out(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot display a value of type %s", "gtrgm")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

static TRGM *
gtrgm_alloc(bool isalltrue, int siglen, BITVECP sign)
{
	int			flag = SIGNKEY | (isalltrue ? ALLISTRUE : 0);
	int			size = CALCGTSIZE(flag, siglen);
	TRGM	   *res = palloc(size);

	SET_VARSIZE(res, size);
	res->flag = flag;

	if (!isalltrue)
	{
		if (sign)
			memcpy(GETSIGN(res), sign, siglen);
		else
			memset(GETSIGN(res), 0, siglen);
	}

	return res;
}

static void
makesign(BITVECP sign, TRGM *a, int siglen)
{
	int32		k,
				len = ARRNELEM(a);
	trgm	   *ptr = GETARR(a);
	int32		tmp = 0;

	MemSet(sign, 0, siglen);
	SETBIT(sign, SIGLENBIT(siglen));	/* set last unused bit */
	for (k = 0; k < len; k++)
	{
		CPTRGM(&tmp, ptr + k);
		HASH(sign, tmp, siglen);
	}
}

Datum
gtrgm_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	int			siglen = GET_SIGLEN();
	GISTENTRY  *retval = entry;

	if (entry->leafkey)
	{							/* trgm */
		TRGM	   *res;
		text	   *val = DatumGetTextPP(entry->key);

		res = generate_trgm(VARDATA_ANY(val), VARSIZE_ANY_EXHDR(val));
		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset, false);
	}
	else if (ISSIGNKEY(DatumGetPointer(entry->key)) &&
			 !ISALLTRUE(DatumGetPointer(entry->key)))
	{
		int32		i;
		TRGM	   *res;
		BITVECP		sign = GETSIGN(DatumGetPointer(entry->key));

		LOOPBYTE(siglen)
		{
			if ((sign[i] & 0xff) != 0xff)
				PG_RETURN_POINTER(retval);
		}

		res = gtrgm_alloc(true, siglen, sign);
		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset, false);
	}
	PG_RETURN_POINTER(retval);
}

Datum
gtrgm_decompress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;
	text	   *key;

	key = DatumGetTextPP(entry->key);

	if (key != (text *) DatumGetPointer(entry->key))
	{
		/* need to pass back the decompressed item */
		retval = palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page, entry->offset, entry->leafkey);
		PG_RETURN_POINTER(retval);
	}
	else
	{
		/* we can return the entry as-is */
		PG_RETURN_POINTER(entry);
	}
}

static int32
cnt_sml_sign_common(TRGM *qtrg, BITVECP sign, int siglen)
{
	int32		count = 0;
	int32		k,
				len = ARRNELEM(qtrg);
	trgm	   *ptr = GETARR(qtrg);
	int32		tmp = 0;

	for (k = 0; k < len; k++)
	{
		CPTRGM(&tmp, ptr + k);
		count += GETBIT(sign, HASHVAL(tmp, siglen));
	}

	return count;
}

Datum
gtrgm_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	text	   *query = PG_GETARG_TEXT_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	int			siglen = GET_SIGLEN();
	TRGM	   *key = (TRGM *) DatumGetPointer(entry->key);
	TRGM	   *qtrg;
	bool		res;
	Size		querysize = VARSIZE(query);
	gtrgm_consistent_cache *cache;
	double		nlimit;

	/*
	 * We keep the extracted trigrams in cache, because trigram extraction is
	 * relatively CPU-expensive.  When trying to reuse a cached value, check
	 * strategy number not just query itself, because trigram extraction
	 * depends on strategy.
	 *
	 * The cached structure is a single palloc chunk containing the
	 * gtrgm_consistent_cache header, then the input query (4-byte length
	 * word, uncompressed, starting at a MAXALIGN boundary), then the TRGM
	 * value (also starting at a MAXALIGN boundary).  However we don't try to
	 * include the regex graph (if any) in that struct.  (XXX currently, this
	 * approach can leak regex graphs across index rescans.  Not clear if
	 * that's worth fixing.)
	 */
	cache = (gtrgm_consistent_cache *) fcinfo->flinfo->fn_extra;
	if (cache == NULL ||
		cache->strategy != strategy ||
		VARSIZE(cache->query) != querysize ||
		memcmp(cache->query, query, querysize) != 0)
	{
		gtrgm_consistent_cache *newcache;
		TrgmPackedGraph *graph = NULL;
		Size		qtrgsize;

		switch (strategy)
		{
			case SimilarityStrategyNumber:
			case WordSimilarityStrategyNumber:
			case StrictWordSimilarityStrategyNumber:
			case EqualStrategyNumber:
				qtrg = generate_trgm(VARDATA(query),
									 querysize - VARHDRSZ);
				break;
			case ILikeStrategyNumber:
#ifndef IGNORECASE
				elog(ERROR, "cannot handle ~~* with case-sensitive trigrams");
#endif
				/* FALL THRU */
			case LikeStrategyNumber:
				qtrg = generate_wildcard_trgm(VARDATA(query),
											  querysize - VARHDRSZ);
				break;
			case RegExpICaseStrategyNumber:
#ifndef IGNORECASE
				elog(ERROR, "cannot handle ~* with case-sensitive trigrams");
#endif
				/* FALL THRU */
			case RegExpStrategyNumber:
				qtrg = createTrgmNFA(query, PG_GET_COLLATION(),
									 &graph, fcinfo->flinfo->fn_mcxt);
				/* just in case an empty array is returned ... */
				if (qtrg && ARRNELEM(qtrg) <= 0)
				{
					pfree(qtrg);
					qtrg = NULL;
				}
				break;
			default:
				elog(ERROR, "unrecognized strategy number: %d", strategy);
				qtrg = NULL;	/* keep compiler quiet */
				break;
		}

		qtrgsize = qtrg ? VARSIZE(qtrg) : 0;

		newcache = (gtrgm_consistent_cache *)
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   MAXALIGN(sizeof(gtrgm_consistent_cache)) +
							   MAXALIGN(querysize) +
							   qtrgsize);

		newcache->strategy = strategy;
		newcache->query = (text *)
			((char *) newcache + MAXALIGN(sizeof(gtrgm_consistent_cache)));
		memcpy(newcache->query, query, querysize);
		if (qtrg)
		{
			newcache->trigrams = (TRGM *)
				((char *) newcache->query + MAXALIGN(querysize));
			memcpy((char *) newcache->trigrams, qtrg, qtrgsize);
			/* release qtrg in case it was made in fn_mcxt */
			pfree(qtrg);
		}
		else
			newcache->trigrams = NULL;
		newcache->graph = graph;

		if (cache)
			pfree(cache);
		fcinfo->flinfo->fn_extra = newcache;
		cache = newcache;
	}

	qtrg = cache->trigrams;

	switch (strategy)
	{
		case SimilarityStrategyNumber:
		case WordSimilarityStrategyNumber:
		case StrictWordSimilarityStrategyNumber:

			/*
			 * Similarity search is exact. (Strict) word similarity search is
			 * inexact
			 */
			*recheck = (strategy != SimilarityStrategyNumber);

			nlimit = index_strategy_get_limit(strategy);

			if (GIST_LEAF(entry))
			{					/* all leafs contains orig trgm */
				double		tmpsml = cnt_sml(qtrg, key, *recheck);

				res = (tmpsml >= nlimit);
			}
			else if (ISALLTRUE(key))
			{					/* non-leaf contains signature */
				res = true;
			}
			else
			{					/* non-leaf contains signature */
				int32		count = cnt_sml_sign_common(qtrg, GETSIGN(key), siglen);
				int32		len = ARRNELEM(qtrg);

				if (len == 0)
					res = false;
				else
					res = (((((float8) count) / ((float8) len))) >= nlimit);
			}
			break;
		case ILikeStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~~* with case-sensitive trigrams");
#endif
			/* FALL THRU */
		case LikeStrategyNumber:
		case EqualStrategyNumber:
			/* Wildcard and equal search are inexact */
			*recheck = true;

			/*
			 * Check if all the extracted trigrams can be present in child
			 * nodes.
			 */
			if (GIST_LEAF(entry))
			{					/* all leafs contains orig trgm */
				res = trgm_contained_by(qtrg, key);
			}
			else if (ISALLTRUE(key))
			{					/* non-leaf contains signature */
				res = true;
			}
			else
			{					/* non-leaf contains signature */
				int32		k,
							tmp = 0,
							len = ARRNELEM(qtrg);
				trgm	   *ptr = GETARR(qtrg);
				BITVECP		sign = GETSIGN(key);

				res = true;
				for (k = 0; k < len; k++)
				{
					CPTRGM(&tmp, ptr + k);
					if (!GETBIT(sign, HASHVAL(tmp, siglen)))
					{
						res = false;
						break;
					}
				}
			}
			break;
		case RegExpICaseStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~* with case-sensitive trigrams");
#endif
			/* FALL THRU */
		case RegExpStrategyNumber:
			/* Regexp search is inexact */
			*recheck = true;

			/* Check regex match as much as we can with available info */
			if (qtrg)
			{
				if (GIST_LEAF(entry))
				{				/* all leafs contains orig trgm */
					bool	   *check;

					check = trgm_presence_map(qtrg, key);
					res = trigramsMatchGraph(cache->graph, check);
					pfree(check);
				}
				else if (ISALLTRUE(key))
				{				/* non-leaf contains signature */
					res = true;
				}
				else
				{				/* non-leaf contains signature */
					int32		k,
								tmp = 0,
								len = ARRNELEM(qtrg);
					trgm	   *ptr = GETARR(qtrg);
					BITVECP		sign = GETSIGN(key);
					bool	   *check;

					/*
					 * GETBIT() tests may give false positives, due to limited
					 * size of the sign array.  But since trigramsMatchGraph()
					 * implements a monotone boolean function, false positives
					 * in the check array can't lead to false negative answer.
					 * So we can apply trigramsMatchGraph despite uncertainty,
					 * and that usefully improves the quality of the search.
					 */
					check = (bool *) palloc(len * sizeof(bool));
					for (k = 0; k < len; k++)
					{
						CPTRGM(&tmp, ptr + k);
						check[k] = GETBIT(sign, HASHVAL(tmp, siglen));
					}
					res = trigramsMatchGraph(cache->graph, check);
					pfree(check);
				}
			}
			else
			{
				/* trigram-free query must be rechecked everywhere */
				res = true;
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			res = false;		/* keep compiler quiet */
			break;
	}

	PG_RETURN_BOOL(res);
}

Datum
gtrgm_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	text	   *query = PG_GETARG_TEXT_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	int			siglen = GET_SIGLEN();
	TRGM	   *key = (TRGM *) DatumGetPointer(entry->key);
	TRGM	   *qtrg;
	float8		res;
	Size		querysize = VARSIZE(query);
	char	   *cache = (char *) fcinfo->flinfo->fn_extra;

	/*
	 * Cache the generated trigrams across multiple calls with the same query.
	 */
	if (cache == NULL ||
		VARSIZE(cache) != querysize ||
		memcmp(cache, query, querysize) != 0)
	{
		char	   *newcache;

		qtrg = generate_trgm(VARDATA(query), querysize - VARHDRSZ);

		newcache = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
									  MAXALIGN(querysize) +
									  VARSIZE(qtrg));

		memcpy(newcache, query, querysize);
		memcpy(newcache + MAXALIGN(querysize), qtrg, VARSIZE(qtrg));

		if (cache)
			pfree(cache);
		fcinfo->flinfo->fn_extra = newcache;
		cache = newcache;
	}

	qtrg = (TRGM *) (cache + MAXALIGN(querysize));

	switch (strategy)
	{
		case DistanceStrategyNumber:
		case WordDistanceStrategyNumber:
		case StrictWordDistanceStrategyNumber:
			/* Only plain trigram distance is exact */
			*recheck = (strategy != DistanceStrategyNumber);
			if (GIST_LEAF(entry))
			{					/* all leafs contains orig trgm */

				/*
				 * Prevent gcc optimizing the sml variable using volatile
				 * keyword. Otherwise res can differ from the
				 * word_similarity_dist_op() function.
				 */
				float4 volatile sml = cnt_sml(qtrg, key, *recheck);

				res = 1.0 - sml;
			}
			else if (ISALLTRUE(key))
			{					/* all leafs contains orig trgm */
				res = 0.0;
			}
			else
			{					/* non-leaf contains signature */
				int32		count = cnt_sml_sign_common(qtrg, GETSIGN(key), siglen);
				int32		len = ARRNELEM(qtrg);

				res = (len == 0) ? -1.0 : 1.0 - ((float8) count) / ((float8) len);
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			res = 0;			/* keep compiler quiet */
			break;
	}

	PG_RETURN_FLOAT8(res);
}

static int32
unionkey(BITVECP sbase, TRGM *add, int siglen)
{
	int32		i;

	if (ISSIGNKEY(add))
	{
		BITVECP		sadd = GETSIGN(add);

		if (ISALLTRUE(add))
			return 1;

		LOOPBYTE(siglen)
			sbase[i] |= sadd[i];
	}
	else
	{
		trgm	   *ptr = GETARR(add);
		int32		tmp = 0;

		for (i = 0; i < ARRNELEM(add); i++)
		{
			CPTRGM(&tmp, ptr + i);
			HASH(sbase, tmp, siglen);
		}
	}
	return 0;
}


Datum
gtrgm_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int32		len = entryvec->n;
	int		   *size = (int *) PG_GETARG_POINTER(1);
	int			siglen = GET_SIGLEN();
	int32		i;
	TRGM	   *result = gtrgm_alloc(false, siglen, NULL);
	BITVECP		base = GETSIGN(result);

	for (i = 0; i < len; i++)
	{
		if (unionkey(base, GETENTRY(entryvec, i), siglen))
		{
			result->flag = ALLISTRUE;
			SET_VARSIZE(result, CALCGTSIZE(ALLISTRUE, siglen));
			break;
		}
	}

	*size = VARSIZE(result);

	PG_RETURN_POINTER(result);
}

Datum
gtrgm_same(PG_FUNCTION_ARGS)
{
	TRGM	   *a = (TRGM *) PG_GETARG_POINTER(0);
	TRGM	   *b = (TRGM *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);
	int			siglen = GET_SIGLEN();

	if (ISSIGNKEY(a))
	{							/* then b also ISSIGNKEY */
		if (ISALLTRUE(a) && ISALLTRUE(b))
			*result = true;
		else if (ISALLTRUE(a))
			*result = false;
		else if (ISALLTRUE(b))
			*result = false;
		else
		{
			int32		i;
			BITVECP		sa = GETSIGN(a),
						sb = GETSIGN(b);

			*result = true;
			LOOPBYTE(siglen)
			{
				if (sa[i] != sb[i])
				{
					*result = false;
					break;
				}
			}
		}
	}
	else
	{							/* a and b ISARRKEY */
		int32		lena = ARRNELEM(a),
					lenb = ARRNELEM(b);

		if (lena != lenb)
			*result = false;
		else
		{
			trgm	   *ptra = GETARR(a),
					   *ptrb = GETARR(b);
			int32		i;

			*result = true;
			for (i = 0; i < lena; i++)
				if (CMPTRGM(ptra + i, ptrb + i))
				{
					*result = false;
					break;
				}
		}
	}

	PG_RETURN_POINTER(result);
}

static int32
sizebitvec(BITVECP sign, int siglen)
{
	return pg_popcount(sign, siglen);
}

static int
hemdistsign(BITVECP a, BITVECP b, int siglen)
{
	int			i,
				diff,
				dist = 0;

	LOOPBYTE(siglen)
	{
		diff = (unsigned char) (a[i] ^ b[i]);
		/* Using the popcount functions here isn't likely to win */
		dist += pg_number_of_ones[diff];
	}
	return dist;
}

static int
hemdist(TRGM *a, TRGM *b, int siglen)
{
	if (ISALLTRUE(a))
	{
		if (ISALLTRUE(b))
			return 0;
		else
			return SIGLENBIT(siglen) - sizebitvec(GETSIGN(b), siglen);
	}
	else if (ISALLTRUE(b))
		return SIGLENBIT(siglen) - sizebitvec(GETSIGN(a), siglen);

	return hemdistsign(GETSIGN(a), GETSIGN(b), siglen);
}

Datum
gtrgm_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0); /* always ISSIGNKEY */
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	int			siglen = GET_SIGLEN();
	TRGM	   *origval = (TRGM *) DatumGetPointer(origentry->key);
	TRGM	   *newval = (TRGM *) DatumGetPointer(newentry->key);
	BITVECP		orig = GETSIGN(origval);

	*penalty = 0.0;

	if (ISARRKEY(newval))
	{
		char	   *cache = (char *) fcinfo->flinfo->fn_extra;
		TRGM	   *cachedVal = (TRGM *) (cache + MAXALIGN(siglen));
		Size		newvalsize = VARSIZE(newval);
		BITVECP		sign;

		/*
		 * Cache the sign data across multiple calls with the same newval.
		 */
		if (cache == NULL ||
			VARSIZE(cachedVal) != newvalsize ||
			memcmp(cachedVal, newval, newvalsize) != 0)
		{
			char	   *newcache;

			newcache = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
										  MAXALIGN(siglen) +
										  newvalsize);

			makesign((BITVECP) newcache, newval, siglen);

			cachedVal = (TRGM *) (newcache + MAXALIGN(siglen));
			memcpy(cachedVal, newval, newvalsize);

			if (cache)
				pfree(cache);
			fcinfo->flinfo->fn_extra = newcache;
			cache = newcache;
		}

		sign = (BITVECP) cache;

		if (ISALLTRUE(origval))
			*penalty = ((float) (SIGLENBIT(siglen) - sizebitvec(sign, siglen))) / (float) (SIGLENBIT(siglen) + 1);
		else
			*penalty = hemdistsign(sign, orig, siglen);
	}
	else
		*penalty = hemdist(origval, newval, siglen);
	PG_RETURN_POINTER(penalty);
}

typedef struct
{
	bool		allistrue;
	BITVECP		sign;
} CACHESIGN;

static void
fillcache(CACHESIGN *item, TRGM *key, BITVECP sign, int siglen)
{
	item->allistrue = false;
	item->sign = sign;
	if (ISARRKEY(key))
		makesign(item->sign, key, siglen);
	else if (ISALLTRUE(key))
		item->allistrue = true;
	else
		memcpy(item->sign, GETSIGN(key), siglen);
}

#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )
typedef struct
{
	OffsetNumber pos;
	int32		cost;
} SPLITCOST;

static int
comparecost(const void *a, const void *b)
{
	if (((const SPLITCOST *) a)->cost == ((const SPLITCOST *) b)->cost)
		return 0;
	else
		return (((const SPLITCOST *) a)->cost > ((const SPLITCOST *) b)->cost) ? 1 : -1;
}


static int
hemdistcache(CACHESIGN *a, CACHESIGN *b, int siglen)
{
	if (a->allistrue)
	{
		if (b->allistrue)
			return 0;
		else
			return SIGLENBIT(siglen) - sizebitvec(b->sign, siglen);
	}
	else if (b->allistrue)
		return SIGLENBIT(siglen) - sizebitvec(a->sign, siglen);

	return hemdistsign(a->sign, b->sign, siglen);
}

Datum
gtrgm_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	OffsetNumber maxoff = entryvec->n - 1;
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	int			siglen = GET_SIGLEN();
	OffsetNumber k,
				j;
	TRGM	   *datum_l,
			   *datum_r;
	BITVECP		union_l,
				union_r;
	int32		size_alpha,
				size_beta;
	int32		size_waste,
				waste = -1;
	int32		nbytes;
	OffsetNumber seed_1 = 0,
				seed_2 = 0;
	OffsetNumber *left,
			   *right;
	BITVECP		ptr;
	int			i;
	CACHESIGN  *cache;
	char	   *cache_sign;
	SPLITCOST  *costvector;

	/* cache the sign data for each existing item */
	cache = (CACHESIGN *) palloc(sizeof(CACHESIGN) * (maxoff + 1));
	cache_sign = palloc(siglen * (maxoff + 1));

	for (k = FirstOffsetNumber; k <= maxoff; k = OffsetNumberNext(k))
		fillcache(&cache[k], GETENTRY(entryvec, k), &cache_sign[siglen * k],
				  siglen);

	/* now find the two furthest-apart items */
	for (k = FirstOffsetNumber; k < maxoff; k = OffsetNumberNext(k))
	{
		for (j = OffsetNumberNext(k); j <= maxoff; j = OffsetNumberNext(j))
		{
			size_waste = hemdistcache(&(cache[j]), &(cache[k]), siglen);
			if (size_waste > waste)
			{
				waste = size_waste;
				seed_1 = k;
				seed_2 = j;
			}
		}
	}

	/* just in case we didn't make a selection ... */
	if (seed_1 == 0 || seed_2 == 0)
	{
		seed_1 = 1;
		seed_2 = 2;
	}

	/* initialize the result vectors */
	nbytes = maxoff * sizeof(OffsetNumber);
	v->spl_left = left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = right = (OffsetNumber *) palloc(nbytes);
	v->spl_nleft = 0;
	v->spl_nright = 0;

	/* form initial .. */
	datum_l = gtrgm_alloc(cache[seed_1].allistrue, siglen, cache[seed_1].sign);
	datum_r = gtrgm_alloc(cache[seed_2].allistrue, siglen, cache[seed_2].sign);

	union_l = GETSIGN(datum_l);
	union_r = GETSIGN(datum_r);

	/* sort before ... */
	costvector = (SPLITCOST *) palloc(sizeof(SPLITCOST) * maxoff);
	for (j = FirstOffsetNumber; j <= maxoff; j = OffsetNumberNext(j))
	{
		costvector[j - 1].pos = j;
		size_alpha = hemdistcache(&(cache[seed_1]), &(cache[j]), siglen);
		size_beta = hemdistcache(&(cache[seed_2]), &(cache[j]), siglen);
		costvector[j - 1].cost = abs(size_alpha - size_beta);
	}
	qsort(costvector, maxoff, sizeof(SPLITCOST), comparecost);

	for (k = 0; k < maxoff; k++)
	{
		j = costvector[k].pos;
		if (j == seed_1)
		{
			*left++ = j;
			v->spl_nleft++;
			continue;
		}
		else if (j == seed_2)
		{
			*right++ = j;
			v->spl_nright++;
			continue;
		}

		if (ISALLTRUE(datum_l) || cache[j].allistrue)
		{
			if (ISALLTRUE(datum_l) && cache[j].allistrue)
				size_alpha = 0;
			else
				size_alpha = SIGLENBIT(siglen) -
					sizebitvec((cache[j].allistrue) ? GETSIGN(datum_l) :
							   GETSIGN(cache[j].sign),
							   siglen);
		}
		else
			size_alpha = hemdistsign(cache[j].sign, GETSIGN(datum_l), siglen);

		if (ISALLTRUE(datum_r) || cache[j].allistrue)
		{
			if (ISALLTRUE(datum_r) && cache[j].allistrue)
				size_beta = 0;
			else
				size_beta = SIGLENBIT(siglen) -
					sizebitvec((cache[j].allistrue) ? GETSIGN(datum_r) :
							   GETSIGN(cache[j].sign),
							   siglen);
		}
		else
			size_beta = hemdistsign(cache[j].sign, GETSIGN(datum_r), siglen);

		if (size_alpha < size_beta + WISH_F(v->spl_nleft, v->spl_nright, 0.1))
		{
			if (ISALLTRUE(datum_l) || cache[j].allistrue)
			{
				if (!ISALLTRUE(datum_l))
					memset(GETSIGN(datum_l), 0xff, siglen);
			}
			else
			{
				ptr = cache[j].sign;
				LOOPBYTE(siglen)
					union_l[i] |= ptr[i];
			}
			*left++ = j;
			v->spl_nleft++;
		}
		else
		{
			if (ISALLTRUE(datum_r) || cache[j].allistrue)
			{
				if (!ISALLTRUE(datum_r))
					memset(GETSIGN(datum_r), 0xff, siglen);
			}
			else
			{
				ptr = cache[j].sign;
				LOOPBYTE(siglen)
					union_r[i] |= ptr[i];
			}
			*right++ = j;
			v->spl_nright++;
		}
	}

	v->spl_ldatum = PointerGetDatum(datum_l);
	v->spl_rdatum = PointerGetDatum(datum_r);

	PG_RETURN_POINTER(v);
}

Datum
gtrgm_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(TrgmGistOptions));
	add_local_int_reloption(relopts, "siglen",
							"signature length in bytes",
							SIGLEN_DEFAULT, 1, SIGLEN_MAX,
							offsetof(TrgmGistOptions, siglen));

	PG_RETURN_VOID();
}
