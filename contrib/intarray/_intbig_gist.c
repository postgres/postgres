/*
 * contrib/intarray/_intbig_gist.c
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/skey.h"

#include "_int.h"

#define GETENTRY(vec,pos) ((GISTTYPE *) DatumGetPointer((vec)->vector[(pos)].key))
/*
** _intbig methods
*/
PG_FUNCTION_INFO_V1(g_intbig_consistent);
PG_FUNCTION_INFO_V1(g_intbig_compress);
PG_FUNCTION_INFO_V1(g_intbig_decompress);
PG_FUNCTION_INFO_V1(g_intbig_penalty);
PG_FUNCTION_INFO_V1(g_intbig_picksplit);
PG_FUNCTION_INFO_V1(g_intbig_union);
PG_FUNCTION_INFO_V1(g_intbig_same);

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

PG_FUNCTION_INFO_V1(_intbig_in);
PG_FUNCTION_INFO_V1(_intbig_out);

Datum
_intbig_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("_intbig_in() not implemented")));
	PG_RETURN_DATUM(0);
}

Datum
_intbig_out(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("_intbig_out() not implemented")));
	PG_RETURN_DATUM(0);
}


/*********************************************************************
** intbig functions
*********************************************************************/
static bool
_intbig_overlap(GISTTYPE *a, ArrayType *b)
{
	int			num = ARRNELEMS(b);
	int32	   *ptr = ARRPTR(b);

	CHECKARRVALID(b);

	while (num--)
	{
		if (GETBIT(GETSIGN(a), HASHVAL(*ptr)))
			return true;
		ptr++;
	}

	return false;
}

static bool
_intbig_contains(GISTTYPE *a, ArrayType *b)
{
	int			num = ARRNELEMS(b);
	int32	   *ptr = ARRPTR(b);

	CHECKARRVALID(b);

	while (num--)
	{
		if (!GETBIT(GETSIGN(a), HASHVAL(*ptr)))
			return false;
		ptr++;
	}

	return true;
}

Datum
g_intbig_same(PG_FUNCTION_ARGS)
{
	GISTTYPE   *a = (GISTTYPE *) PG_GETARG_POINTER(0);
	GISTTYPE   *b = (GISTTYPE *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

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
		LOOPBYTE
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

Datum
g_intbig_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	if (entry->leafkey)
	{
		GISTENTRY  *retval;
		ArrayType  *in = DatumGetArrayTypeP(entry->key);
		int32	   *ptr;
		int			num;
		GISTTYPE   *res = (GISTTYPE *) palloc0(CALCGTSIZE(0));

		CHECKARRVALID(in);
		if (ARRISEMPTY(in))
		{
			ptr = NULL;
			num = 0;
		}
		else
		{
			ptr = ARRPTR(in);
			num = ARRNELEMS(in);
		}
		SET_VARSIZE(res, CALCGTSIZE(0));

		while (num--)
		{
			HASH(GETSIGN(res), *ptr);
			ptr++;
		}

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset, FALSE);

		if (in != DatumGetArrayTypeP(entry->key))
			pfree(in);

		PG_RETURN_POINTER(retval);
	}
	else if (!ISALLTRUE(DatumGetPointer(entry->key)))
	{
		GISTENTRY  *retval;
		int			i;
		BITVECP		sign = GETSIGN(DatumGetPointer(entry->key));
		GISTTYPE   *res;

		LOOPBYTE
		{
			if ((sign[i] & 0xff) != 0xff)
				PG_RETURN_POINTER(entry);
		}

		res = (GISTTYPE *) palloc(CALCGTSIZE(ALLISTRUE));
		SET_VARSIZE(res, CALCGTSIZE(ALLISTRUE));
		res->flag = ALLISTRUE;

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset, FALSE);

		PG_RETURN_POINTER(retval);
	}

	PG_RETURN_POINTER(entry);
}


static int32
sizebitvec(BITVECP sign)
{
	int32		size = 0,
				i;

	LOOPBYTE
		size += number_of_ones[(unsigned char) sign[i]];
	return size;
}

static int
hemdistsign(BITVECP a, BITVECP b)
{
	int			i,
				diff,
				dist = 0;

	LOOPBYTE
	{
		diff = (unsigned char) (a[i] ^ b[i]);
		dist += number_of_ones[diff];
	}
	return dist;
}

static int
hemdist(GISTTYPE *a, GISTTYPE *b)
{
	if (ISALLTRUE(a))
	{
		if (ISALLTRUE(b))
			return 0;
		else
			return SIGLENBIT - sizebitvec(GETSIGN(b));
	}
	else if (ISALLTRUE(b))
		return SIGLENBIT - sizebitvec(GETSIGN(a));

	return hemdistsign(GETSIGN(a), GETSIGN(b));
}

Datum
g_intbig_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}

static int32
unionkey(BITVECP sbase, GISTTYPE *add)
{
	int32		i;
	BITVECP		sadd = GETSIGN(add);

	if (ISALLTRUE(add))
		return 1;
	LOOPBYTE
		sbase[i] |= sadd[i];
	return 0;
}

Datum
g_intbig_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *size = (int *) PG_GETARG_POINTER(1);
	BITVEC		base;
	int32		i,
				len;
	int32		flag = 0;
	GISTTYPE   *result;

	MemSet((void *) base, 0, sizeof(BITVEC));
	for (i = 0; i < entryvec->n; i++)
	{
		if (unionkey(base, GETENTRY(entryvec, i)))
		{
			flag = ALLISTRUE;
			break;
		}
	}

	len = CALCGTSIZE(flag);
	result = (GISTTYPE *) palloc(len);
	SET_VARSIZE(result, len);
	result->flag = flag;
	if (!ISALLTRUE(result))
		memcpy((void *) GETSIGN(result), (void *) base, sizeof(BITVEC));
	*size = len;

	PG_RETURN_POINTER(result);
}

Datum
g_intbig_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0); /* always ISSIGNKEY */
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	GISTTYPE   *origval = (GISTTYPE *) DatumGetPointer(origentry->key);
	GISTTYPE   *newval = (GISTTYPE *) DatumGetPointer(newentry->key);

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
g_intbig_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber k,
				j;
	GISTTYPE   *datum_l,
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
	GISTTYPE   *_k,
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
	if (ISALLTRUE(GETENTRY(entryvec, seed_1)))
	{
		datum_l = (GISTTYPE *) palloc(GTHDRSIZE);
		SET_VARSIZE(datum_l, GTHDRSIZE);
		datum_l->flag = ALLISTRUE;
	}
	else
	{
		datum_l = (GISTTYPE *) palloc(GTHDRSIZE + SIGLEN);
		SET_VARSIZE(datum_l, GTHDRSIZE + SIGLEN);
		datum_l->flag = 0;
		memcpy((void *) GETSIGN(datum_l), (void *) GETSIGN(GETENTRY(entryvec, seed_1)), sizeof(BITVEC));
	}
	if (ISALLTRUE(GETENTRY(entryvec, seed_2)))
	{
		datum_r = (GISTTYPE *) palloc(GTHDRSIZE);
		SET_VARSIZE(datum_r, GTHDRSIZE);
		datum_r->flag = ALLISTRUE;
	}
	else
	{
		datum_r = (GISTTYPE *) palloc(GTHDRSIZE + SIGLEN);
		SET_VARSIZE(datum_r, GTHDRSIZE + SIGLEN);
		datum_r->flag = 0;
		memcpy((void *) GETSIGN(datum_r), (void *) GETSIGN(GETENTRY(entryvec, seed_2)), sizeof(BITVEC));
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

	union_l = GETSIGN(datum_l);
	union_r = GETSIGN(datum_r);

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
			if (ISALLTRUE(datum_l) || ISALLTRUE(_j))
			{
				if (!ISALLTRUE(datum_l))
					MemSet((void *) union_l, 0xff, sizeof(BITVEC));
			}
			else
			{
				ptr = GETSIGN(_j);
				LOOPBYTE
					union_l[i] |= ptr[i];
			}
			*left++ = j;
			v->spl_nleft++;
		}
		else
		{
			if (ISALLTRUE(datum_r) || ISALLTRUE(_j))
			{
				if (!ISALLTRUE(datum_r))
					MemSet((void *) union_r, 0xff, sizeof(BITVEC));
			}
			else
			{
				ptr = GETSIGN(_j);
				LOOPBYTE
					union_r[i] |= ptr[i];
			}
			*right++ = j;
			v->spl_nright++;
		}
	}

	*right = *left = FirstOffsetNumber;
	pfree(costvector);

	v->spl_ldatum = PointerGetDatum(datum_l);
	v->spl_rdatum = PointerGetDatum(datum_r);

	PG_RETURN_POINTER(v);
}

Datum
g_intbig_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	ArrayType  *query = PG_GETARG_ARRAYTYPE_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		retval;

	/* All cases served by this function are inexact */
	*recheck = true;

	if (ISALLTRUE(DatumGetPointer(entry->key)))
		PG_RETURN_BOOL(true);

	if (strategy == BooleanSearchStrategy)
	{
		retval = signconsistent((QUERYTYPE *) query,
								GETSIGN(DatumGetPointer(entry->key)),
								false);
		PG_FREE_IF_COPY(query, 1);
		PG_RETURN_BOOL(retval);
	}

	CHECKARRVALID(query);

	switch (strategy)
	{
		case RTOverlapStrategyNumber:
			retval = _intbig_overlap((GISTTYPE *) DatumGetPointer(entry->key), query);
			break;
		case RTSameStrategyNumber:
			if (GIST_LEAF(entry))
			{
				int			i,
							num = ARRNELEMS(query);
				int32	   *ptr = ARRPTR(query);
				BITVEC		qp;
				BITVECP		dq,
							de;

				memset(qp, 0, sizeof(BITVEC));

				while (num--)
				{
					HASH(qp, *ptr);
					ptr++;
				}

				de = GETSIGN((GISTTYPE *) DatumGetPointer(entry->key));
				dq = qp;
				retval = true;
				LOOPBYTE
				{
					if (de[i] != dq[i])
					{
						retval = false;
						break;
					}
				}

			}
			else
				retval = _intbig_contains((GISTTYPE *) DatumGetPointer(entry->key), query);
			break;
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			retval = _intbig_contains((GISTTYPE *) DatumGetPointer(entry->key), query);
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
			if (GIST_LEAF(entry))
			{
				int			i,
							num = ARRNELEMS(query);
				int32	   *ptr = ARRPTR(query);
				BITVEC		qp;
				BITVECP		dq,
							de;

				memset(qp, 0, sizeof(BITVEC));

				while (num--)
				{
					HASH(qp, *ptr);
					ptr++;
				}

				de = GETSIGN((GISTTYPE *) DatumGetPointer(entry->key));
				dq = qp;
				retval = true;
				LOOPBYTE
				{
					if (de[i] & ~dq[i])
					{
						retval = false;
						break;
					}
				}
			}
			else
				retval = _intbig_overlap((GISTTYPE *) DatumGetPointer(entry->key), query);
			break;
		default:
			retval = FALSE;
	}
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(retval);
}
