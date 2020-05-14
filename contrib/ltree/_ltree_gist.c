/*
 * contrib/ltree/_ltree_gist.c
 *
 *
 * GiST support for ltree[]
 * Teodor Sigaev <teodor@stack.net>
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/reloptions.h"
#include "access/stratnum.h"
#include "crc32.h"
#include "ltree.h"
#include "port/pg_bitutils.h"

PG_FUNCTION_INFO_V1(_ltree_compress);
PG_FUNCTION_INFO_V1(_ltree_same);
PG_FUNCTION_INFO_V1(_ltree_union);
PG_FUNCTION_INFO_V1(_ltree_penalty);
PG_FUNCTION_INFO_V1(_ltree_picksplit);
PG_FUNCTION_INFO_V1(_ltree_consistent);
PG_FUNCTION_INFO_V1(_ltree_gist_options);

#define GETENTRY(vec,pos) ((ltree_gist *) DatumGetPointer((vec)->vector[(pos)].key))
#define NEXTVAL(x) ( (ltree*)( (char*)(x) + INTALIGN( VARSIZE(x) ) ) )

#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )


static void
hashing(BITVECP sign, ltree *t, int siglen)
{
	int			tlen = t->numlevel;
	ltree_level *cur = LTREE_FIRST(t);
	int			hash;

	while (tlen > 0)
	{
		hash = ltree_crc32_sz(cur->name, cur->len);
		AHASH(sign, hash, siglen);
		cur = LEVEL_NEXT(cur);
		tlen--;
	}
}

Datum
_ltree_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = entry;
	int			siglen = LTREE_GET_ASIGLEN();

	if (entry->leafkey)
	{							/* ltree */
		ltree_gist *key;
		ArrayType  *val = DatumGetArrayTypeP(entry->key);
		int			num = ArrayGetNItems(ARR_NDIM(val), ARR_DIMS(val));
		ltree	   *item = (ltree *) ARR_DATA_PTR(val);

		if (ARR_NDIM(val) > 1)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("array must be one-dimensional")));
		if (array_contains_nulls(val))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("array must not contain nulls")));

		key = ltree_gist_alloc(false, NULL, siglen, NULL, NULL);

		while (num > 0)
		{
			hashing(LTG_SIGN(key), item, siglen);
			num--;
			item = NEXTVAL(item);
		}

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, false);
	}
	else if (!LTG_ISALLTRUE(entry->key))
	{
		int32		i;
		ltree_gist *key;
		BITVECP		sign = LTG_SIGN(DatumGetPointer(entry->key));

		ALOOPBYTE(siglen)
		{
			if ((sign[i] & 0xff) != 0xff)
				PG_RETURN_POINTER(retval);
		}

		key = ltree_gist_alloc(true, sign, siglen, NULL, NULL);
		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, false);
	}
	PG_RETURN_POINTER(retval);
}

Datum
_ltree_same(PG_FUNCTION_ARGS)
{
	ltree_gist *a = (ltree_gist *) PG_GETARG_POINTER(0);
	ltree_gist *b = (ltree_gist *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);
	int			siglen = LTREE_GET_ASIGLEN();

	if (LTG_ISALLTRUE(a) && LTG_ISALLTRUE(b))
		*result = true;
	else if (LTG_ISALLTRUE(a))
		*result = false;
	else if (LTG_ISALLTRUE(b))
		*result = false;
	else
	{
		int32		i;
		BITVECP		sa = LTG_SIGN(a),
					sb = LTG_SIGN(b);

		*result = true;
		ALOOPBYTE(siglen)
		{
			if (sa[i] != sb[i])
			{
				*result = false;
				break;
			}
		}
	}
	PG_RETURN_POINTER(result);
}

static int32
unionkey(BITVECP sbase, ltree_gist *add, int siglen)
{
	int32		i;
	BITVECP		sadd = LTG_SIGN(add);

	if (LTG_ISALLTRUE(add))
		return 1;

	ALOOPBYTE(siglen)
		sbase[i] |= sadd[i];
	return 0;
}

Datum
_ltree_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *size = (int *) PG_GETARG_POINTER(1);
	int			siglen = LTREE_GET_ASIGLEN();
	int32		i;
	ltree_gist *result = ltree_gist_alloc(false, NULL, siglen, NULL, NULL);
	BITVECP		base = LTG_SIGN(result);

	for (i = 0; i < entryvec->n; i++)
	{
		if (unionkey(base, GETENTRY(entryvec, i), siglen))
		{
			result->flag |= LTG_ALLTRUE;
			SET_VARSIZE(result, LTG_HDRSIZE);
			break;
		}
	}

	*size = VARSIZE(result);

	PG_RETURN_POINTER(result);
}

static int32
sizebitvec(BITVECP sign, int siglen)
{
	return pg_popcount((const char *) sign, siglen);
}

static int
hemdistsign(BITVECP a, BITVECP b, int siglen)
{
	int			i,
				diff,
				dist = 0;

	ALOOPBYTE(siglen)
	{
		diff = (unsigned char) (a[i] ^ b[i]);
		/* Using the popcount functions here isn't likely to win */
		dist += pg_number_of_ones[diff];
	}
	return dist;
}

static int
hemdist(ltree_gist *a, ltree_gist *b, int siglen)
{
	if (LTG_ISALLTRUE(a))
	{
		if (LTG_ISALLTRUE(b))
			return 0;
		else
			return ASIGLENBIT(siglen) - sizebitvec(LTG_SIGN(b), siglen);
	}
	else if (LTG_ISALLTRUE(b))
		return ASIGLENBIT(siglen) - sizebitvec(LTG_SIGN(a), siglen);

	return hemdistsign(LTG_SIGN(a), LTG_SIGN(b), siglen);
}


Datum
_ltree_penalty(PG_FUNCTION_ARGS)
{
	ltree_gist *origval = (ltree_gist *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	ltree_gist *newval = (ltree_gist *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	int			siglen = LTREE_GET_ASIGLEN();

	*penalty = hemdist(origval, newval, siglen);
	PG_RETURN_POINTER(penalty);
}

typedef struct
{
	OffsetNumber pos;
	int32		cost;
} SPLITCOST;

static int
comparecost(const void *a, const void *b)
{
	return ((const SPLITCOST *) a)->cost - ((const SPLITCOST *) b)->cost;
}

Datum
_ltree_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	int			siglen = LTREE_GET_ASIGLEN();
	OffsetNumber k,
				j;
	ltree_gist *datum_l,
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
	OffsetNumber maxoff;
	BITVECP		ptr;
	int			i;
	SPLITCOST  *costvector;
	ltree_gist *_k,
			   *_j;

	maxoff = entryvec->n - 2;
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	for (k = FirstOffsetNumber; k < maxoff; k = OffsetNumberNext(k))
	{
		_k = GETENTRY(entryvec, k);
		for (j = OffsetNumberNext(k); j <= maxoff; j = OffsetNumberNext(j))
		{
			size_waste = hemdist(_k, GETENTRY(entryvec, j), siglen);
			if (size_waste > waste)
			{
				waste = size_waste;
				seed_1 = k;
				seed_2 = j;
			}
		}
	}

	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	if (seed_1 == 0 || seed_2 == 0)
	{
		seed_1 = 1;
		seed_2 = 2;
	}

	/* form initial .. */
	datum_l = ltree_gist_alloc(LTG_ISALLTRUE(GETENTRY(entryvec, seed_1)),
							   LTG_SIGN(GETENTRY(entryvec, seed_1)),
							   siglen, NULL, NULL);

	datum_r = ltree_gist_alloc(LTG_ISALLTRUE(GETENTRY(entryvec, seed_2)),
							   LTG_SIGN(GETENTRY(entryvec, seed_2)),
							   siglen, NULL, NULL);

	maxoff = OffsetNumberNext(maxoff);
	/* sort before ... */
	costvector = (SPLITCOST *) palloc(sizeof(SPLITCOST) * maxoff);
	for (j = FirstOffsetNumber; j <= maxoff; j = OffsetNumberNext(j))
	{
		costvector[j - 1].pos = j;
		_j = GETENTRY(entryvec, j);
		size_alpha = hemdist(datum_l, _j, siglen);
		size_beta = hemdist(datum_r, _j, siglen);
		costvector[j - 1].cost = Abs(size_alpha - size_beta);
	}
	qsort((void *) costvector, maxoff, sizeof(SPLITCOST), comparecost);

	union_l = LTG_SIGN(datum_l);
	union_r = LTG_SIGN(datum_r);

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
		_j = GETENTRY(entryvec, j);
		size_alpha = hemdist(datum_l, _j, siglen);
		size_beta = hemdist(datum_r, _j, siglen);

		if (size_alpha < size_beta + WISH_F(v->spl_nleft, v->spl_nright, 0.00001))
		{
			if (LTG_ISALLTRUE(datum_l) || LTG_ISALLTRUE(_j))
			{
				if (!LTG_ISALLTRUE(datum_l))
					MemSet((void *) union_l, 0xff, siglen);
			}
			else
			{
				ptr = LTG_SIGN(_j);
				ALOOPBYTE(siglen)
					union_l[i] |= ptr[i];
			}
			*left++ = j;
			v->spl_nleft++;
		}
		else
		{
			if (LTG_ISALLTRUE(datum_r) || LTG_ISALLTRUE(_j))
			{
				if (!LTG_ISALLTRUE(datum_r))
					MemSet((void *) union_r, 0xff, siglen);
			}
			else
			{
				ptr = LTG_SIGN(_j);
				ALOOPBYTE(siglen)
					union_r[i] |= ptr[i];
			}
			*right++ = j;
			v->spl_nright++;
		}
	}

	*right = *left = FirstOffsetNumber;

	v->spl_ldatum = PointerGetDatum(datum_l);
	v->spl_rdatum = PointerGetDatum(datum_r);

	PG_RETURN_POINTER(v);
}

static bool
gist_te(ltree_gist *key, ltree *query, int siglen)
{
	ltree_level *curq = LTREE_FIRST(query);
	BITVECP		sign = LTG_SIGN(key);
	int			qlen = query->numlevel;
	unsigned int hv;

	if (LTG_ISALLTRUE(key))
		return true;

	while (qlen > 0)
	{
		hv = ltree_crc32_sz(curq->name, curq->len);
		if (!GETBIT(sign, AHASHVAL(hv, siglen)))
			return false;
		curq = LEVEL_NEXT(curq);
		qlen--;
	}

	return true;
}

typedef struct LtreeSignature
{
	BITVECP		sign;
	int			siglen;
} LtreeSignature;

static bool
checkcondition_bit(void *cxt, ITEM *val)
{
	LtreeSignature *sig = cxt;

	return (FLG_CANLOOKSIGN(val->flag)) ? GETBIT(sig->sign, AHASHVAL(val->val, sig->siglen)) : true;
}

static bool
gist_qtxt(ltree_gist *key, ltxtquery *query, int siglen)
{
	LtreeSignature sig;

	if (LTG_ISALLTRUE(key))
		return true;

	sig.sign = LTG_SIGN(key);
	sig.siglen = siglen;

	return ltree_execute(GETQUERY(query),
						 &sig, false,
						 checkcondition_bit);
}

static bool
gist_qe(ltree_gist *key, lquery *query, int siglen)
{
	lquery_level *curq = LQUERY_FIRST(query);
	BITVECP		sign = LTG_SIGN(key);
	int			qlen = query->numlevel;

	if (LTG_ISALLTRUE(key))
		return true;

	while (qlen > 0)
	{
		if (curq->numvar && LQL_CANLOOKSIGN(curq))
		{
			bool		isexist = false;
			int			vlen = curq->numvar;
			lquery_variant *curv = LQL_FIRST(curq);

			while (vlen > 0)
			{
				if (GETBIT(sign, AHASHVAL(curv->val, siglen)))
				{
					isexist = true;
					break;
				}
				curv = LVAR_NEXT(curv);
				vlen--;
			}
			if (!isexist)
				return false;
		}

		curq = LQL_NEXT(curq);
		qlen--;
	}

	return true;
}

static bool
_arrq_cons(ltree_gist *key, ArrayType *_query, int siglen)
{
	lquery	   *query = (lquery *) ARR_DATA_PTR(_query);
	int			num = ArrayGetNItems(ARR_NDIM(_query), ARR_DIMS(_query));

	if (ARR_NDIM(_query) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array must be one-dimensional")));
	if (array_contains_nulls(_query))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array must not contain nulls")));

	while (num > 0)
	{
		if (gist_qe(key, query, siglen))
			return true;
		num--;
		query = (lquery *) NEXTVAL(query);
	}
	return false;
}

Datum
_ltree_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	void	   *query = (void *) PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	int			siglen = LTREE_GET_ASIGLEN();
	ltree_gist *key = (ltree_gist *) DatumGetPointer(entry->key);
	bool		res = false;

	/* All cases served by this function are inexact */
	*recheck = true;

	switch (strategy)
	{
		case 10:
		case 11:
			res = gist_te(key, (ltree *) query, siglen);
			break;
		case 12:
		case 13:
			res = gist_qe(key, (lquery *) query, siglen);
			break;
		case 14:
		case 15:
			res = gist_qtxt(key, (ltxtquery *) query, siglen);
			break;
		case 16:
		case 17:
			res = _arrq_cons(key, (ArrayType *) query, siglen);
			break;
		default:
			/* internal error */
			elog(ERROR, "unrecognized StrategyNumber: %d", strategy);
	}
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(res);
}

Datum
_ltree_gist_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(LtreeGistOptions));
	add_local_int_reloption(relopts, "siglen", "signature length",
							LTREE_ASIGLEN_DEFAULT, 1, LTREE_ASIGLEN_MAX,
							offsetof(LtreeGistOptions, siglen));

	PG_RETURN_VOID();
}
