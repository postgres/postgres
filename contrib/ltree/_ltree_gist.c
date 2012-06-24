/*
 * contrib/ltree/_ltree_gist.c
 *
 *
 * GiST support for ltree[]
 * Teodor Sigaev <teodor@stack.net>
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/skey.h"
#include "crc32.h"
#include "ltree.h"


PG_FUNCTION_INFO_V1(_ltree_compress);
Datum		_ltree_compress(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(_ltree_same);
Datum		_ltree_same(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(_ltree_union);
Datum		_ltree_union(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(_ltree_penalty);
Datum		_ltree_penalty(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(_ltree_picksplit);
Datum		_ltree_picksplit(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(_ltree_consistent);
Datum		_ltree_consistent(PG_FUNCTION_ARGS);

#define GETENTRY(vec,pos) ((ltree_gist *) DatumGetPointer((vec)->vector[(pos)].key))
#define NEXTVAL(x) ( (ltree*)( (char*)(x) + INTALIGN( VARSIZE(x) ) ) )

/* Number of one-bits in an unsigned byte */
static const uint8 number_of_ones[256] = {
	0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8
};

#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )


static void
hashing(BITVECP sign, ltree *t)
{
	int			tlen = t->numlevel;
	ltree_level *cur = LTREE_FIRST(t);
	int			hash;

	while (tlen > 0)
	{
		hash = ltree_crc32_sz(cur->name, cur->len);
		AHASH(sign, hash);
		cur = LEVEL_NEXT(cur);
		tlen--;
	}
}

Datum
_ltree_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = entry;

	if (entry->leafkey)
	{							/* ltree */
		ltree_gist *key;
		ArrayType  *val = DatumGetArrayTypeP(entry->key);
		int32		len = LTG_HDRSIZE + ASIGLEN;
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

		key = (ltree_gist *) palloc(len);
		SET_VARSIZE(key, len);
		key->flag = 0;

		MemSet(LTG_SIGN(key), 0, ASIGLEN);
		while (num > 0)
		{
			hashing(LTG_SIGN(key), item);
			num--;
			item = NEXTVAL(item);
		}

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
	}
	else if (!LTG_ISALLTRUE(entry->key))
	{
		int32		i,
					len;
		ltree_gist *key;

		BITVECP		sign = LTG_SIGN(DatumGetPointer(entry->key));

		ALOOPBYTE
		{
			if ((sign[i] & 0xff) != 0xff)
				PG_RETURN_POINTER(retval);
		}
		len = LTG_HDRSIZE;
		key = (ltree_gist *) palloc(len);
		SET_VARSIZE(key, len);
		key->flag = LTG_ALLTRUE;

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
	}
	PG_RETURN_POINTER(retval);
}

Datum
_ltree_same(PG_FUNCTION_ARGS)
{
	ltree_gist *a = (ltree_gist *) PG_GETARG_POINTER(0);
	ltree_gist *b = (ltree_gist *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

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
		ALOOPBYTE
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
unionkey(BITVECP sbase, ltree_gist *add)
{
	int32		i;
	BITVECP		sadd = LTG_SIGN(add);

	if (LTG_ISALLTRUE(add))
		return 1;

	ALOOPBYTE
		sbase[i] |= sadd[i];
	return 0;
}

Datum
_ltree_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *size = (int *) PG_GETARG_POINTER(1);
	ABITVEC		base;
	int32		i,
				len;
	int32		flag = 0;
	ltree_gist *result;

	MemSet((void *) base, 0, sizeof(ABITVEC));
	for (i = 0; i < entryvec->n; i++)
	{
		if (unionkey(base, GETENTRY(entryvec, i)))
		{
			flag = LTG_ALLTRUE;
			break;
		}
	}

	len = LTG_HDRSIZE + ((flag & LTG_ALLTRUE) ? 0 : ASIGLEN);
	result = (ltree_gist *) palloc(len);
	SET_VARSIZE(result, len);
	result->flag = flag;
	if (!LTG_ISALLTRUE(result))
		memcpy((void *) LTG_SIGN(result), (void *) base, sizeof(ABITVEC));
	*size = len;

	PG_RETURN_POINTER(result);
}

static int32
sizebitvec(BITVECP sign)
{
	int32		size = 0,
				i;

	ALOOPBYTE
		size += number_of_ones[(unsigned char) sign[i]];
	return size;
}

static int
hemdistsign(BITVECP a, BITVECP b)
{
	int			i,
				diff,
				dist = 0;

	ALOOPBYTE
	{
		diff = (unsigned char) (a[i] ^ b[i]);
		dist += number_of_ones[diff];
	}
	return dist;
}

static int
hemdist(ltree_gist *a, ltree_gist *b)
{
	if (LTG_ISALLTRUE(a))
	{
		if (LTG_ISALLTRUE(b))
			return 0;
		else
			return ASIGLENBIT - sizebitvec(LTG_SIGN(b));
	}
	else if (LTG_ISALLTRUE(b))
		return ASIGLENBIT - sizebitvec(LTG_SIGN(a));

	return hemdistsign(LTG_SIGN(a), LTG_SIGN(b));
}


Datum
_ltree_penalty(PG_FUNCTION_ARGS)
{
	ltree_gist *origval = (ltree_gist *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	ltree_gist *newval = (ltree_gist *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);

	*penalty = hemdist(origval, newval);
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
			size_waste = hemdist(_k, GETENTRY(entryvec, j));
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
	if (LTG_ISALLTRUE(GETENTRY(entryvec, seed_1)))
	{
		datum_l = (ltree_gist *) palloc(LTG_HDRSIZE);
		SET_VARSIZE(datum_l, LTG_HDRSIZE);
		datum_l->flag = LTG_ALLTRUE;
	}
	else
	{
		datum_l = (ltree_gist *) palloc(LTG_HDRSIZE + ASIGLEN);
		SET_VARSIZE(datum_l, LTG_HDRSIZE + ASIGLEN);
		datum_l->flag = 0;
		memcpy((void *) LTG_SIGN(datum_l), (void *) LTG_SIGN(GETENTRY(entryvec, seed_1)), sizeof(ABITVEC));
	}
	if (LTG_ISALLTRUE(GETENTRY(entryvec, seed_2)))
	{
		datum_r = (ltree_gist *) palloc(LTG_HDRSIZE);
		SET_VARSIZE(datum_r, LTG_HDRSIZE);
		datum_r->flag = LTG_ALLTRUE;
	}
	else
	{
		datum_r = (ltree_gist *) palloc(LTG_HDRSIZE + ASIGLEN);
		SET_VARSIZE(datum_r, LTG_HDRSIZE + ASIGLEN);
		datum_r->flag = 0;
		memcpy((void *) LTG_SIGN(datum_r), (void *) LTG_SIGN(GETENTRY(entryvec, seed_2)), sizeof(ABITVEC));
	}

	maxoff = OffsetNumberNext(maxoff);
	/* sort before ... */
	costvector = (SPLITCOST *) palloc(sizeof(SPLITCOST) * maxoff);
	for (j = FirstOffsetNumber; j <= maxoff; j = OffsetNumberNext(j))
	{
		costvector[j - 1].pos = j;
		_j = GETENTRY(entryvec, j);
		size_alpha = hemdist(datum_l, _j);
		size_beta = hemdist(datum_r, _j);
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
		size_alpha = hemdist(datum_l, _j);
		size_beta = hemdist(datum_r, _j);

		if (size_alpha < size_beta + WISH_F(v->spl_nleft, v->spl_nright, 0.00001))
		{
			if (LTG_ISALLTRUE(datum_l) || LTG_ISALLTRUE(_j))
			{
				if (!LTG_ISALLTRUE(datum_l))
					MemSet((void *) union_l, 0xff, sizeof(ABITVEC));
			}
			else
			{
				ptr = LTG_SIGN(_j);
				ALOOPBYTE
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
					MemSet((void *) union_r, 0xff, sizeof(ABITVEC));
			}
			else
			{
				ptr = LTG_SIGN(_j);
				ALOOPBYTE
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
gist_te(ltree_gist *key, ltree *query)
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
		if (!GETBIT(sign, AHASHVAL(hv)))
			return false;
		curq = LEVEL_NEXT(curq);
		qlen--;
	}

	return true;
}

static bool
checkcondition_bit(void *checkval, ITEM *val)
{
	return (FLG_CANLOOKSIGN(val->flag)) ? GETBIT(checkval, AHASHVAL(val->val)) : true;
}

static bool
gist_qtxt(ltree_gist *key, ltxtquery *query)
{
	if (LTG_ISALLTRUE(key))
		return true;

	return ltree_execute(
						 GETQUERY(query),
						 (void *) LTG_SIGN(key), false,
						 checkcondition_bit
		);
}

static bool
gist_qe(ltree_gist *key, lquery *query)
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
				if (GETBIT(sign, AHASHVAL(curv->val)))
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
_arrq_cons(ltree_gist *key, ArrayType *_query)
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
		if (gist_qe(key, query))
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
	char	   *query = (char *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(1)));
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	ltree_gist *key = (ltree_gist *) DatumGetPointer(entry->key);
	bool		res = false;

	/* All cases served by this function are inexact */
	*recheck = true;

	switch (strategy)
	{
		case 10:
		case 11:
			res = gist_te(key, (ltree *) query);
			break;
		case 12:
		case 13:
			res = gist_qe(key, (lquery *) query);
			break;
		case 14:
		case 15:
			res = gist_qtxt(key, (ltxtquery *) query);
			break;
		case 16:
		case 17:
			res = _arrq_cons(key, (ArrayType *) query);
			break;
		default:
			/* internal error */
			elog(ERROR, "unrecognized StrategyNumber: %d", strategy);
	}
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(res);
}
