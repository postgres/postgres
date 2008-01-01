/*-------------------------------------------------------------------------
 *
 * tsquery_gist.c
 *	  GiST index support for tsquery
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/tsquery_gist.c,v 1.4 2008/01/01 19:45:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/skey.h"
#include "access/gist.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"

#define GETENTRY(vec,pos) ((TSQuerySign *) DatumGetPointer((vec)->vector[(pos)].key))

Datum
gtsquery_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = entry;

	if (entry->leafkey)
	{
		TSQuerySign *sign = (TSQuerySign *) palloc(sizeof(TSQuerySign));

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		*sign = makeTSQuerySign(DatumGetTSQuery(entry->key));

		gistentryinit(*retval, PointerGetDatum(sign),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
	}

	PG_RETURN_POINTER(retval);
}

Datum
gtsquery_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}

Datum
gtsquery_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	TSQuerySign *key = (TSQuerySign *) DatumGetPointer(entry->key);
	TSQuery		query = PG_GETARG_TSQUERY(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	TSQuerySign sq = makeTSQuerySign(query);
	bool		retval;

	switch (strategy)
	{
		case RTContainsStrategyNumber:
			if (GIST_LEAF(entry))
				retval = (*key & sq) == sq;
			else
				retval = (*key & sq) != 0;
			break;
		case RTContainedByStrategyNumber:
			if (GIST_LEAF(entry))
				retval = (*key & sq) == *key;
			else
				retval = (*key & sq) != 0;
			break;
		default:
			retval = FALSE;
	}
	PG_RETURN_BOOL(retval);
}

Datum
gtsquery_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *size = (int *) PG_GETARG_POINTER(1);
	TSQuerySign *sign = (TSQuerySign *) palloc(sizeof(TSQuerySign));
	int			i;

	memset(sign, 0, sizeof(TSQuerySign));

	for (i = 0; i < entryvec->n; i++)
		*sign |= *GETENTRY(entryvec, i);

	*size = sizeof(TSQuerySign);

	PG_RETURN_POINTER(sign);
}

Datum
gtsquery_same(PG_FUNCTION_ARGS)
{
	TSQuerySign *a = (TSQuerySign *) PG_GETARG_POINTER(0);
	TSQuerySign *b = (TSQuerySign *) PG_GETARG_POINTER(1);

	PG_RETURN_POINTER(*a == *b);
}

static int
sizebitvec(TSQuerySign sign)
{
	int			size = 0,
				i;

	for (i = 0; i < TSQS_SIGLEN; i++)
		size += 0x01 & (sign >> i);

	return size;
}

static int
hemdist(TSQuerySign a, TSQuerySign b)
{
	TSQuerySign res = a ^ b;

	return sizebitvec(res);
}

Datum
gtsquery_penalty(PG_FUNCTION_ARGS)
{
	TSQuerySign *origval = (TSQuerySign *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	TSQuerySign *newval = (TSQuerySign *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);

	*penalty = hemdist(*origval, *newval);

	PG_RETURN_POINTER(penalty);
}


typedef struct
{
	OffsetNumber pos;
	int4		cost;
} SPLITCOST;

static int
comparecost(const void *a, const void *b)
{
	if (((SPLITCOST *) a)->cost == ((SPLITCOST *) b)->cost)
		return 0;
	else
		return (((SPLITCOST *) a)->cost > ((SPLITCOST *) b)->cost) ? 1 : -1;
}

#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )

Datum
gtsquery_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber maxoff = entryvec->n - 2;
	OffsetNumber k,
				j;

	TSQuerySign *datum_l,
			   *datum_r;
	int4		size_alpha,
				size_beta;
	int4		size_waste,
				waste = -1;
	int4		nbytes;
	OffsetNumber seed_1 = 0,
				seed_2 = 0;
	OffsetNumber *left,
			   *right;

	SPLITCOST  *costvector;

	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	left = v->spl_left = (OffsetNumber *) palloc(nbytes);
	right = v->spl_right = (OffsetNumber *) palloc(nbytes);
	v->spl_nleft = v->spl_nright = 0;

	for (k = FirstOffsetNumber; k < maxoff; k = OffsetNumberNext(k))
		for (j = OffsetNumberNext(k); j <= maxoff; j = OffsetNumberNext(j))
		{
			size_waste = hemdist(*GETENTRY(entryvec, j), *GETENTRY(entryvec, k));
			if (size_waste > waste)
			{
				waste = size_waste;
				seed_1 = k;
				seed_2 = j;
			}
		}


	if (seed_1 == 0 || seed_2 == 0)
	{
		seed_1 = 1;
		seed_2 = 2;
	}

	datum_l = (TSQuerySign *) palloc(sizeof(TSQuerySign));
	*datum_l = *GETENTRY(entryvec, seed_1);
	datum_r = (TSQuerySign *) palloc(sizeof(TSQuerySign));
	*datum_r = *GETENTRY(entryvec, seed_2);


	maxoff = OffsetNumberNext(maxoff);
	costvector = (SPLITCOST *) palloc(sizeof(SPLITCOST) * maxoff);
	for (j = FirstOffsetNumber; j <= maxoff; j = OffsetNumberNext(j))
	{
		costvector[j - 1].pos = j;
		size_alpha = hemdist(*GETENTRY(entryvec, seed_1), *GETENTRY(entryvec, j));
		size_beta = hemdist(*GETENTRY(entryvec, seed_2), *GETENTRY(entryvec, j));
		costvector[j - 1].cost = abs(size_alpha - size_beta);
	}
	qsort((void *) costvector, maxoff, sizeof(SPLITCOST), comparecost);

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
		size_alpha = hemdist(*datum_l, *GETENTRY(entryvec, j));
		size_beta = hemdist(*datum_r, *GETENTRY(entryvec, j));

		if (size_alpha < size_beta + WISH_F(v->spl_nleft, v->spl_nright, 0.05))
		{
			*datum_l |= *GETENTRY(entryvec, j);
			*left++ = j;
			v->spl_nleft++;
		}
		else
		{
			*datum_r |= *GETENTRY(entryvec, j);
			*right++ = j;
			v->spl_nright++;
		}
	}

	*right = *left = FirstOffsetNumber;
	v->spl_ldatum = PointerGetDatum(datum_l);
	v->spl_rdatum = PointerGetDatum(datum_r);

	PG_RETURN_POINTER(v);
}
