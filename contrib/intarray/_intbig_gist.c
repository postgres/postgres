/*
 * contrib/intarray/_intbig_gist.c
 */
#include "postgres.h"

#include "_int.h"
#include "access/gist.h"
#include "access/reloptions.h"
#include "access/stratnum.h"
#include "port/pg_bitutils.h"

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
PG_FUNCTION_INFO_V1(g_intbig_options);

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

static GISTTYPE *
_intbig_alloc(bool allistrue, int siglen, BITVECP sign)
{
	int			flag = allistrue ? ALLISTRUE : 0;
	int			size = CALCGTSIZE(flag, siglen);
	GISTTYPE   *res = (GISTTYPE *) palloc(size);

	SET_VARSIZE(res, size);
	res->flag = flag;

	if (!allistrue)
	{
		if (sign)
			memcpy(GETSIGN(res), sign, siglen);
		else
			memset(GETSIGN(res), 0, siglen);
	}

	return res;
}


/*********************************************************************
** intbig functions
*********************************************************************/
static bool
_intbig_overlap(GISTTYPE *a, ArrayType *b, int siglen)
{
	int			num = ARRNELEMS(b);
	int32	   *ptr = ARRPTR(b);

	CHECKARRVALID(b);

	while (num--)
	{
		if (GETBIT(GETSIGN(a), HASHVAL(*ptr, siglen)))
			return true;
		ptr++;
	}

	return false;
}

static bool
_intbig_contains(GISTTYPE *a, ArrayType *b, int siglen)
{
	int			num = ARRNELEMS(b);
	int32	   *ptr = ARRPTR(b);

	CHECKARRVALID(b);

	while (num--)
	{
		if (!GETBIT(GETSIGN(a), HASHVAL(*ptr, siglen)))
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
	int			siglen = GET_SIGLEN();

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
	PG_RETURN_POINTER(result);
}

Datum
g_intbig_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	int			siglen = GET_SIGLEN();

	if (entry->leafkey)
	{
		GISTENTRY  *retval;
		ArrayType  *in = DatumGetArrayTypeP(entry->key);
		int32	   *ptr;
		int			num;
		GISTTYPE   *res = _intbig_alloc(false, siglen, NULL);

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

		while (num--)
		{
			HASH(GETSIGN(res), *ptr, siglen);
			ptr++;
		}

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset, false);

		PG_RETURN_POINTER(retval);
	}
	else if (!ISALLTRUE(DatumGetPointer(entry->key)))
	{
		GISTENTRY  *retval;
		int			i;
		BITVECP		sign = GETSIGN(DatumGetPointer(entry->key));
		GISTTYPE   *res;

		LOOPBYTE(siglen)
		{
			if ((sign[i] & 0xff) != 0xff)
				PG_RETURN_POINTER(entry);
		}

		res = _intbig_alloc(true, siglen, sign);
		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset, false);

		PG_RETURN_POINTER(retval);
	}

	PG_RETURN_POINTER(entry);
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
hemdist(GISTTYPE *a, GISTTYPE *b, int siglen)
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
g_intbig_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}

static int32
unionkey(BITVECP sbase, GISTTYPE *add, int siglen)
{
	int32		i;
	BITVECP		sadd = GETSIGN(add);

	if (ISALLTRUE(add))
		return 1;
	LOOPBYTE(siglen)
		sbase[i] |= sadd[i];
	return 0;
}

Datum
g_intbig_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *size = (int *) PG_GETARG_POINTER(1);
	int			siglen = GET_SIGLEN();
	int32		i;
	GISTTYPE   *result = _intbig_alloc(false, siglen, NULL);
	BITVECP		base = GETSIGN(result);

	for (i = 0; i < entryvec->n; i++)
	{
		if (unionkey(base, GETENTRY(entryvec, i), siglen))
		{
			result->flag |= ALLISTRUE;
			SET_VARSIZE(result, CALCGTSIZE(ALLISTRUE, siglen));
			break;
		}
	}

	*size = VARSIZE(result);

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
	int			siglen = GET_SIGLEN();

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
g_intbig_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	int			siglen = GET_SIGLEN();
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
	datum_l = _intbig_alloc(ISALLTRUE(GETENTRY(entryvec, seed_1)), siglen,
							GETSIGN(GETENTRY(entryvec, seed_1)));
	datum_r = _intbig_alloc(ISALLTRUE(GETENTRY(entryvec, seed_2)), siglen,
							GETSIGN(GETENTRY(entryvec, seed_2)));

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
		size_alpha = hemdist(datum_l, _j, siglen);
		size_beta = hemdist(datum_r, _j, siglen);

		if (size_alpha < size_beta + WISH_F(v->spl_nleft, v->spl_nright, 0.00001))
		{
			if (ISALLTRUE(datum_l) || ISALLTRUE(_j))
			{
				if (!ISALLTRUE(datum_l))
					MemSet((void *) union_l, 0xff, siglen);
			}
			else
			{
				ptr = GETSIGN(_j);
				LOOPBYTE(siglen)
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
					MemSet((void *) union_r, 0xff, siglen);
			}
			else
			{
				ptr = GETSIGN(_j);
				LOOPBYTE(siglen)
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
	int			siglen = GET_SIGLEN();
	bool		retval;

	/* All cases served by this function are inexact */
	*recheck = true;

	if (ISALLTRUE(DatumGetPointer(entry->key)))
		PG_RETURN_BOOL(true);

	if (strategy == BooleanSearchStrategy)
	{
		retval = signconsistent((QUERYTYPE *) query,
								GETSIGN(DatumGetPointer(entry->key)),
								siglen,
								false);
		PG_FREE_IF_COPY(query, 1);
		PG_RETURN_BOOL(retval);
	}

	CHECKARRVALID(query);

	switch (strategy)
	{
		case RTOverlapStrategyNumber:
			retval = _intbig_overlap((GISTTYPE *) DatumGetPointer(entry->key),
									 query, siglen);
			break;
		case RTSameStrategyNumber:
			if (GIST_LEAF(entry))
			{
				int			i,
							num = ARRNELEMS(query);
				int32	   *ptr = ARRPTR(query);
				BITVECP		dq = palloc0(siglen),
							de;

				while (num--)
				{
					HASH(dq, *ptr, siglen);
					ptr++;
				}

				de = GETSIGN((GISTTYPE *) DatumGetPointer(entry->key));
				retval = true;
				LOOPBYTE(siglen)
				{
					if (de[i] != dq[i])
					{
						retval = false;
						break;
					}
				}

				pfree(dq);
			}
			else
				retval = _intbig_contains((GISTTYPE *) DatumGetPointer(entry->key),
										  query, siglen);
			break;
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			retval = _intbig_contains((GISTTYPE *) DatumGetPointer(entry->key),
									  query, siglen);
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
			if (GIST_LEAF(entry))
			{
				int			i,
							num = ARRNELEMS(query);
				int32	   *ptr = ARRPTR(query);
				BITVECP		dq = palloc0(siglen),
							de;

				while (num--)
				{
					HASH(dq, *ptr, siglen);
					ptr++;
				}

				de = GETSIGN((GISTTYPE *) DatumGetPointer(entry->key));
				retval = true;
				LOOPBYTE(siglen)
				{
					if (de[i] & ~dq[i])
					{
						retval = false;
						break;
					}
				}
			}
			else
			{
				/*
				 * Unfortunately, because empty arrays could be anywhere in
				 * the index, we must search the whole tree.
				 */
				retval = true;
			}
			break;
		default:
			retval = false;
	}
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(retval);
}

Datum
g_intbig_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(GISTIntArrayBigOptions));
	add_local_int_reloption(relopts, "siglen",
							"signature length in bytes",
							SIGLEN_DEFAULT, 1, SIGLEN_MAX,
							offsetof(GISTIntArrayBigOptions, siglen));

	PG_RETURN_VOID();
}
