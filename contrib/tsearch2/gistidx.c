/* $PostgreSQL: pgsql/contrib/tsearch2/gistidx.c,v 1.15 2006/10/04 00:29:46 momjian Exp $ */

#include "postgres.h"

#include <float.h>

#include "access/gist.h"
#include "access/itup.h"
#include "access/tuptoaster.h"
#include "storage/bufpage.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "tsvector.h"
#include "query.h"
#include "gistidx.h"
#include "crc32.h"

PG_FUNCTION_INFO_V1(gtsvector_in);
Datum		gtsvector_in(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsvector_out);
Datum		gtsvector_out(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsvector_compress);
Datum		gtsvector_compress(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsvector_decompress);
Datum		gtsvector_decompress(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsvector_consistent);
Datum		gtsvector_consistent(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsvector_union);
Datum		gtsvector_union(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsvector_same);
Datum		gtsvector_same(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsvector_penalty);
Datum		gtsvector_penalty(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtsvector_picksplit);
Datum		gtsvector_picksplit(PG_FUNCTION_ARGS);

#define GETENTRY(vec,pos) ((GISTTYPE *) DatumGetPointer((vec)->vector[(pos)].key))

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

static int4 sizebitvec(BITVECP sign);

Datum
gtsvector_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("gtsvector_in not implemented")));
	PG_RETURN_DATUM(0);
}

#define SINGOUTSTR	"%d true bits, %d false bits"
#define ARROUTSTR	"%d unique words"
#define EXTRALEN	( 2*13 )

static int	outbuf_maxlen = 0;

Datum
gtsvector_out(PG_FUNCTION_ARGS)
{
	GISTTYPE   *key = (GISTTYPE *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_POINTER(0)));
	char	   *outbuf;

	if (outbuf_maxlen == 0)
		outbuf_maxlen = 2 * EXTRALEN + Max(strlen(SINGOUTSTR), strlen(ARROUTSTR)) + 1;
	outbuf = palloc(outbuf_maxlen);

	if (ISARRKEY(key))
		sprintf(outbuf, ARROUTSTR, (int) ARRNELEM(key));
	else
	{
		int			cnttrue = (ISALLTRUE(key)) ? SIGLENBIT : sizebitvec(GETSIGN(key));

		sprintf(outbuf, SINGOUTSTR, cnttrue, (int) SIGLENBIT - cnttrue);
	}

	PG_FREE_IF_COPY(key, 0);
	PG_RETURN_POINTER(outbuf);
}

static int
compareint(const void *a, const void *b)
{
	if (*((int4 *) a) == *((int4 *) b))
		return 0;
	return (*((int4 *) a) > *((int4 *) b)) ? 1 : -1;
}

static int
uniqueint(int4 *a, int4 l)
{
	int4	   *ptr,
			   *res;

	if (l == 1)
		return l;

	ptr = res = a;

	qsort((void *) a, l, sizeof(int4), compareint);

	while (ptr - a < l)
		if (*ptr != *res)
			*(++res) = *ptr++;
		else
			ptr++;
	return res + 1 - a;
}

static void
makesign(BITVECP sign, GISTTYPE * a)
{
	int4		k,
				len = ARRNELEM(a);
	int4	   *ptr = GETARR(a);

	MemSet((void *) sign, 0, sizeof(BITVEC));
	for (k = 0; k < len; k++)
		HASH(sign, ptr[k]);
}

Datum
gtsvector_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = entry;

	if (entry->leafkey)
	{							/* tsvector */
		GISTTYPE   *res;
		tsvector   *val = (tsvector *) DatumGetPointer(PG_DETOAST_DATUM(entry->key));
		int4		len;
		int4	   *arr;
		WordEntry  *ptr = ARRPTR(val);
		char	   *words = STRPTR(val);

		len = CALCGTSIZE(ARRKEY, val->size);
		res = (GISTTYPE *) palloc(len);
		res->len = len;
		res->flag = ARRKEY;
		arr = GETARR(res);
		len = val->size;
		while (len--)
		{
			*arr = crc32_sz(&words[ptr->pos], ptr->len);
			arr++;
			ptr++;
		}

		len = uniqueint(GETARR(res), val->size);
		if (len != val->size)
		{
			/*
			 * there is a collision of hash-function; len is always less than
			 * val->size
			 */
			len = CALCGTSIZE(ARRKEY, len);
			res = (GISTTYPE *) repalloc((void *) res, len);
			res->len = len;
		}

		/* make signature, if array is too long */
		if (res->len > TOAST_INDEX_TARGET)
		{
			GISTTYPE   *ressign;

			len = CALCGTSIZE(SIGNKEY, 0);
			ressign = (GISTTYPE *) palloc(len);
			ressign->len = len;
			ressign->flag = SIGNKEY;
			makesign(GETSIGN(ressign), res);
			res = ressign;
		}

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
	}
	else if (ISSIGNKEY(DatumGetPointer(entry->key)) &&
			 !ISALLTRUE(DatumGetPointer(entry->key)))
	{
		int4		i,
					len;
		GISTTYPE   *res;
		BITVECP		sign = GETSIGN(DatumGetPointer(entry->key));

		LOOPBYTE(
				 if ((sign[i] & 0xff) != 0xff)
				 PG_RETURN_POINTER(retval);
		);

		len = CALCGTSIZE(SIGNKEY | ALLISTRUE, 0);
		res = (GISTTYPE *) palloc(len);
		res->len = len;
		res->flag = SIGNKEY | ALLISTRUE;

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
	}
	PG_RETURN_POINTER(retval);
}

Datum
gtsvector_decompress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTTYPE   *key = (GISTTYPE *) DatumGetPointer(PG_DETOAST_DATUM(entry->key));

	if (key != (GISTTYPE *) DatumGetPointer(entry->key))
	{
		GISTENTRY  *retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));

		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, FALSE);

		PG_RETURN_POINTER(retval);
	}

	PG_RETURN_POINTER(entry);
}

typedef struct
{
	int4	   *arrb;
	int4	   *arre;
}	CHKVAL;

/*
 * is there value 'val' in array or not ?
 */
static bool
checkcondition_arr(void *checkval, ITEM * val)
{
	int4	   *StopLow = ((CHKVAL *) checkval)->arrb;
	int4	   *StopHigh = ((CHKVAL *) checkval)->arre;
	int4	   *StopMiddle;

	/* Loop invariant: StopLow <= val < StopHigh */

	while (StopLow < StopHigh)
	{
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		if (*StopMiddle == val->val)
			return (true);
		else if (*StopMiddle < val->val)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}

	return (false);
}

static bool
checkcondition_bit(void *checkval, ITEM * val)
{
	return GETBIT(checkval, HASHVAL(val->val));
}

Datum
gtsvector_consistent(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *query = (QUERYTYPE *) PG_GETARG_POINTER(1);
	GISTTYPE   *key = (GISTTYPE *) DatumGetPointer(
									((GISTENTRY *) PG_GETARG_POINTER(0))->key
	);

	if (!query->size)
		PG_RETURN_BOOL(false);

	if (ISSIGNKEY(key))
	{
		if (ISALLTRUE(key))
			PG_RETURN_BOOL(true);

		PG_RETURN_BOOL(TS_execute(
								  GETQUERY(query),
								  (void *) GETSIGN(key), false,
								  checkcondition_bit
								  ));
	}
	else
	{							/* only leaf pages */
		CHKVAL		chkval;

		chkval.arrb = GETARR(key);
		chkval.arre = chkval.arrb + ARRNELEM(key);
		PG_RETURN_BOOL(TS_execute(
								  GETQUERY(query),
								  (void *) &chkval, true,
								  checkcondition_arr
								  ));
	}
}

static int4
unionkey(BITVECP sbase, GISTTYPE * add)
{
	int4		i;

	if (ISSIGNKEY(add))
	{
		BITVECP		sadd = GETSIGN(add);

		if (ISALLTRUE(add))
			return 1;

		LOOPBYTE(
				 sbase[i] |= sadd[i];
		);
	}
	else
	{
		int4	   *ptr = GETARR(add);

		for (i = 0; i < ARRNELEM(add); i++)
			HASH(sbase, ptr[i]);
	}
	return 0;
}


Datum
gtsvector_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *size = (int *) PG_GETARG_POINTER(1);
	BITVEC		base;
	int4		i,
				len;
	int4		flag = 0;
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

	flag |= SIGNKEY;
	len = CALCGTSIZE(flag, 0);
	result = (GISTTYPE *) palloc(len);
	*size = result->len = len;
	result->flag = flag;
	if (!ISALLTRUE(result))
		memcpy((void *) GETSIGN(result), (void *) base, sizeof(BITVEC));

	PG_RETURN_POINTER(result);
}

Datum
gtsvector_same(PG_FUNCTION_ARGS)
{
	GISTTYPE   *a = (GISTTYPE *) PG_GETARG_POINTER(0);
	GISTTYPE   *b = (GISTTYPE *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

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
			int4		i;
			BITVECP		sa = GETSIGN(a),
						sb = GETSIGN(b);

			*result = true;
			LOOPBYTE(
					 if (sa[i] != sb[i])
					 {
				*result = false;
				break;
			}
			);
		}
	}
	else
	{							/* a and b ISARRKEY */
		int4		lena = ARRNELEM(a),
					lenb = ARRNELEM(b);

		if (lena != lenb)
			*result = false;
		else
		{
			int4	   *ptra = GETARR(a),
					   *ptrb = GETARR(b);
			int4		i;

			*result = true;
			for (i = 0; i < lena; i++)
				if (ptra[i] != ptrb[i])
				{
					*result = false;
					break;
				}
		}
	}

	PG_RETURN_POINTER(result);
}

static int4
sizebitvec(BITVECP sign)
{
	int4		size = 0,
				i;

	LOOPBYTE(
			 size += number_of_ones[(unsigned char) sign[i]];
	);
	return size;
}

static int
hemdistsign(BITVECP a, BITVECP b)
{
	int			i,
				diff,
				dist = 0;

	LOOPBYTE(
			 diff = (unsigned char) (a[i] ^ b[i]);
	dist += number_of_ones[diff];
	);
	return dist;
}

static int
hemdist(GISTTYPE * a, GISTTYPE * b)
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
gtsvector_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0); /* always ISSIGNKEY */
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	GISTTYPE   *origval = (GISTTYPE *) DatumGetPointer(origentry->key);
	GISTTYPE   *newval = (GISTTYPE *) DatumGetPointer(newentry->key);
	BITVECP		orig = GETSIGN(origval);

	*penalty = 0.0;

	if (ISARRKEY(newval))
	{
		BITVEC		sign;

		makesign(sign, newval);

		if (ISALLTRUE(origval))
			*penalty = ((float) (SIGLENBIT - sizebitvec(sign))) / (float) (SIGLENBIT + 1);
		else
			*penalty = hemdistsign(sign, orig);
	}
	else
		*penalty = hemdist(origval, newval);
	PG_RETURN_POINTER(penalty);
}

typedef struct
{
	bool		allistrue;
	BITVEC		sign;
}	CACHESIGN;

static void
fillcache(CACHESIGN * item, GISTTYPE * key)
{
	item->allistrue = false;
	if (ISARRKEY(key))
		makesign(item->sign, key);
	else if (ISALLTRUE(key))
		item->allistrue = true;
	else
		memcpy((void *) item->sign, (void *) GETSIGN(key), sizeof(BITVEC));
}

#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )
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


static int
hemdistcache(CACHESIGN * a, CACHESIGN * b)
{
	if (a->allistrue)
	{
		if (b->allistrue)
			return 0;
		else
			return SIGLENBIT - sizebitvec(b->sign);
	}
	else if (b->allistrue)
		return SIGLENBIT - sizebitvec(a->sign);

	return hemdistsign(a->sign, b->sign);
}

Datum
gtsvector_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber k,
				j;
	GISTTYPE   *datum_l,
			   *datum_r;
	BITVECP		union_l,
				union_r;
	int4		size_alpha,
				size_beta;
	int4		size_waste,
				waste = -1;
	int4		nbytes;
	OffsetNumber seed_1 = 0,
				seed_2 = 0;
	OffsetNumber *left,
			   *right;
	OffsetNumber maxoff;
	BITVECP		ptr;
	int			i;
	CACHESIGN  *cache;
	SPLITCOST  *costvector;

	maxoff = entryvec->n - 2;
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	cache = (CACHESIGN *) palloc(sizeof(CACHESIGN) * (maxoff + 2));
	fillcache(&cache[FirstOffsetNumber], GETENTRY(entryvec, FirstOffsetNumber));

	for (k = FirstOffsetNumber; k < maxoff; k = OffsetNumberNext(k))
	{
		for (j = OffsetNumberNext(k); j <= maxoff; j = OffsetNumberNext(j))
		{
			if (k == FirstOffsetNumber)
				fillcache(&cache[j], GETENTRY(entryvec, j));

			size_waste = hemdistcache(&(cache[j]), &(cache[k]));
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
	if (cache[seed_1].allistrue)
	{
		datum_l = (GISTTYPE *) palloc(CALCGTSIZE(SIGNKEY | ALLISTRUE, 0));
		datum_l->len = CALCGTSIZE(SIGNKEY | ALLISTRUE, 0);
		datum_l->flag = SIGNKEY | ALLISTRUE;
	}
	else
	{
		datum_l = (GISTTYPE *) palloc(CALCGTSIZE(SIGNKEY, 0));
		datum_l->len = CALCGTSIZE(SIGNKEY, 0);
		datum_l->flag = SIGNKEY;
		memcpy((void *) GETSIGN(datum_l), (void *) cache[seed_1].sign, sizeof(BITVEC));
	}
	if (cache[seed_2].allistrue)
	{
		datum_r = (GISTTYPE *) palloc(CALCGTSIZE(SIGNKEY | ALLISTRUE, 0));
		datum_r->len = CALCGTSIZE(SIGNKEY | ALLISTRUE, 0);
		datum_r->flag = SIGNKEY | ALLISTRUE;
	}
	else
	{
		datum_r = (GISTTYPE *) palloc(CALCGTSIZE(SIGNKEY, 0));
		datum_r->len = CALCGTSIZE(SIGNKEY, 0);
		datum_r->flag = SIGNKEY;
		memcpy((void *) GETSIGN(datum_r), (void *) cache[seed_2].sign, sizeof(BITVEC));
	}

	union_l = GETSIGN(datum_l);
	union_r = GETSIGN(datum_r);
	maxoff = OffsetNumberNext(maxoff);
	fillcache(&cache[maxoff], GETENTRY(entryvec, maxoff));
	/* sort before ... */
	costvector = (SPLITCOST *) palloc(sizeof(SPLITCOST) * maxoff);
	for (j = FirstOffsetNumber; j <= maxoff; j = OffsetNumberNext(j))
	{
		costvector[j - 1].pos = j;
		size_alpha = hemdistcache(&(cache[seed_1]), &(cache[j]));
		size_beta = hemdistcache(&(cache[seed_2]), &(cache[j]));
		costvector[j - 1].cost = Abs(size_alpha - size_beta);
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

		if (ISALLTRUE(datum_l) || cache[j].allistrue)
		{
			if (ISALLTRUE(datum_l) && cache[j].allistrue)
				size_alpha = 0;
			else
				size_alpha = SIGLENBIT - sizebitvec(
													(cache[j].allistrue) ? GETSIGN(datum_l) : GETSIGN(cache[j].sign)
					);
		}
		else
			size_alpha = hemdistsign(cache[j].sign, GETSIGN(datum_l));

		if (ISALLTRUE(datum_r) || cache[j].allistrue)
		{
			if (ISALLTRUE(datum_r) && cache[j].allistrue)
				size_beta = 0;
			else
				size_beta = SIGLENBIT - sizebitvec(
												   (cache[j].allistrue) ? GETSIGN(datum_r) : GETSIGN(cache[j].sign)
					);
		}
		else
			size_beta = hemdistsign(cache[j].sign, GETSIGN(datum_r));

		if (size_alpha < size_beta + WISH_F(v->spl_nleft, v->spl_nright, 0.1))
		{
			if (ISALLTRUE(datum_l) || cache[j].allistrue)
			{
				if (!ISALLTRUE(datum_l))
					MemSet((void *) GETSIGN(datum_l), 0xff, sizeof(BITVEC));
			}
			else
			{
				ptr = cache[j].sign;
				LOOPBYTE(
						 union_l[i] |= ptr[i];
				);
			}
			*left++ = j;
			v->spl_nleft++;
		}
		else
		{
			if (ISALLTRUE(datum_r) || cache[j].allistrue)
			{
				if (!ISALLTRUE(datum_r))
					MemSet((void *) GETSIGN(datum_r), 0xff, sizeof(BITVEC));
			}
			else
			{
				ptr = cache[j].sign;
				LOOPBYTE(
						 union_r[i] |= ptr[i];
				);
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
