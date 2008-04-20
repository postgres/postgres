#include "postgres.h"

#include "access/skey.h"
#include "storage/bufpage.h"
#include "access/gist.h"

#include "query.h"

typedef uint64 TPQTGist;

#define SIGLEN	(sizeof(TPQTGist)*BITS_PER_BYTE)


#define GETENTRY(vec,pos) ((TPQTGist *) DatumGetPointer((vec)->vector[(pos)].key))

PG_FUNCTION_INFO_V1(tsq_mcontains);
Datum		tsq_mcontains(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(tsq_mcontained);
Datum		tsq_mcontained(PG_FUNCTION_ARGS);

static TPQTGist
makesign(QUERYTYPE * a)
{
	int			i;
	ITEM	   *ptr = GETQUERY(a);
	TPQTGist	sign = 0;

	for (i = 0; i < a->size; i++)
	{
		if (ptr->type == VAL)
			sign |= ((TPQTGist) 1) << (ptr->val % SIGLEN);
		ptr++;
	}

	return sign;
}

Datum
tsq_mcontains(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *query = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	QUERYTYPE  *ex = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(1)));
	TPQTGist	sq,
				se;
	int			i,
				j;
	ITEM	   *iq,
			   *ie;

	if (query->size < ex->size)
	{
		PG_FREE_IF_COPY(query, 0);
		PG_FREE_IF_COPY(ex, 1);

		PG_RETURN_BOOL(false);
	}

	sq = makesign(query);
	se = makesign(ex);

	if ((sq & se) != se)
	{
		PG_FREE_IF_COPY(query, 0);
		PG_FREE_IF_COPY(ex, 1);

		PG_RETURN_BOOL(false);
	}

	ie = GETQUERY(ex);

	for (i = 0; i < ex->size; i++)
	{
		iq = GETQUERY(query);
		if (ie[i].type != VAL)
			continue;
		for (j = 0; j < query->size; j++)
			if (iq[j].type == VAL && ie[i].val == iq[j].val)
			{
				j = query->size + 1;
				break;
			}
		if (j == query->size)
		{
			PG_FREE_IF_COPY(query, 0);
			PG_FREE_IF_COPY(ex, 1);

			PG_RETURN_BOOL(false);
		}
	}

	PG_FREE_IF_COPY(query, 0);
	PG_FREE_IF_COPY(ex, 1);

	PG_RETURN_BOOL(true);
}

Datum
tsq_mcontained(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(
					DirectFunctionCall2(
										tsq_mcontains,
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(0)
										)
		);
}

PG_FUNCTION_INFO_V1(gtsq_in);
Datum		gtsq_in(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsq_out);
Datum		gtsq_out(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsq_compress);
Datum		gtsq_compress(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsq_decompress);
Datum		gtsq_decompress(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsq_consistent);
Datum		gtsq_consistent(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsq_union);
Datum		gtsq_union(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsq_same);
Datum		gtsq_same(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsq_penalty);
Datum		gtsq_penalty(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsq_picksplit);
Datum		gtsq_picksplit(PG_FUNCTION_ARGS);


Datum
gtsq_in(PG_FUNCTION_ARGS)
{
	elog(ERROR, "not implemented");
	PG_RETURN_DATUM(0);
}

Datum
gtsq_out(PG_FUNCTION_ARGS)
{
	elog(ERROR, "not implemented");
	PG_RETURN_DATUM(0);
}

Datum
gtsq_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = entry;

	if (entry->leafkey)
	{
		TPQTGist   *sign = (TPQTGist *) palloc(sizeof(TPQTGist));

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		*sign = makesign((QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM(entry->key)));

		gistentryinit(*retval, PointerGetDatum(sign),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
	}

	PG_RETURN_POINTER(retval);
}

Datum
gtsq_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}

Datum
gtsq_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	TPQTGist   *key = (TPQTGist *) DatumGetPointer(entry->key);
	QUERYTYPE  *query = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(1)));
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	TPQTGist	sq = makesign(query);
	bool		retval;

	switch (strategy)
	{
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			if (GIST_LEAF(entry))
				retval = (*key & sq) == sq;
			else
				retval = (*key & sq) != 0;
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
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
gtsq_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	TPQTGist   *sign = (TPQTGist *) palloc(sizeof(TPQTGist));
	int			i;
	int		   *size = (int *) PG_GETARG_POINTER(1);

	memset(sign, 0, sizeof(TPQTGist));

	for (i = 0; i < entryvec->n; i++)
		*sign |= *GETENTRY(entryvec, i);

	*size = sizeof(TPQTGist);

	PG_RETURN_POINTER(sign);
}

Datum
gtsq_same(PG_FUNCTION_ARGS)
{
	TPQTGist   *a = (TPQTGist *) PG_GETARG_POINTER(0);
	TPQTGist   *b = (TPQTGist *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = (*a == *b) ? true : false;

	PG_RETURN_POINTER(result);
}

static int
sizebitvec(TPQTGist sign)
{
	int			size = 0,
				i;

	for (i = 0; i < SIGLEN; i++)
		size += 0x01 & (sign >> i);

	return size;
}

static int
hemdist(TPQTGist a, TPQTGist b)
{
	TPQTGist	res = a ^ b;

	return sizebitvec(res);
}

Datum
gtsq_penalty(PG_FUNCTION_ARGS)
{
	TPQTGist   *origval = (TPQTGist *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	TPQTGist   *newval = (TPQTGist *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);

	*penalty = hemdist(*origval, *newval);

	PG_RETURN_POINTER(penalty);
}


typedef struct
{
	OffsetNumber pos;
	int4		cost;
}	SPLITCOST;

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
gtsq_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber maxoff = entryvec->n - 2;
	OffsetNumber k,
				j;

	TPQTGist   *datum_l,
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

	datum_l = (TPQTGist *) palloc(sizeof(TPQTGist));
	*datum_l = *GETENTRY(entryvec, seed_1);
	datum_r = (TPQTGist *) palloc(sizeof(TPQTGist));
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
