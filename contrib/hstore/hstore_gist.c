/*
 * contrib/hstore/hstore_gist.c
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/reloptions.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "hstore.h"
#include "utils/pg_crc.h"

/* gist_hstore_ops opclass options */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			siglen;			/* signature length in bytes */
} GistHstoreOptions;

/* bigint defines */
#define BITBYTE 8
#define SIGLEN_DEFAULT	(sizeof(int32) * 4)
#define SIGLEN_MAX		GISTMaxIndexKeySize
#define SIGLENBIT(siglen) ((siglen) * BITBYTE)
#define GET_SIGLEN()	(PG_HAS_OPCLASS_OPTIONS() ? \
						 ((GistHstoreOptions *) PG_GET_OPCLASS_OPTIONS())->siglen : \
						 SIGLEN_DEFAULT)


typedef char *BITVECP;

#define LOOPBYTE(siglen) \
			for (i = 0; i < (siglen); i++)

#define LOOPBIT(siglen) \
			for (i = 0; i < SIGLENBIT(siglen); i++)

/* beware of multiple evaluation of arguments to these macros! */
#define GETBYTE(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITBYTE ) ) )
#define GETBITBYTE(x,i) ( (*((char*)(x)) >> (i)) & 0x01 )
#define CLRBIT(x,i)   GETBYTE(x,i) &= ~( 0x01 << ( (i) % BITBYTE ) )
#define SETBIT(x,i)   GETBYTE(x,i) |=  ( 0x01 << ( (i) % BITBYTE ) )
#define GETBIT(x,i) ( (GETBYTE(x,i) >> ( (i) % BITBYTE )) & 0x01 )
#define HASHVAL(val, siglen) (((unsigned int)(val)) % SIGLENBIT(siglen))
#define HASH(sign, val, siglen) SETBIT((sign), HASHVAL(val, siglen))

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		flag;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} GISTTYPE;

#define ALLISTRUE		0x04

#define ISALLTRUE(x)	( ((GISTTYPE*)x)->flag & ALLISTRUE )

#define GTHDRSIZE		(VARHDRSZ + sizeof(int32))
#define CALCGTSIZE(flag, siglen) ( GTHDRSIZE+(((flag) & ALLISTRUE) ? 0 : (siglen)) )

#define GETSIGN(x)		( (BITVECP)( (char*)x+GTHDRSIZE ) )

#define SUMBIT(val) (		\
	GETBITBYTE((val),0) + \
	GETBITBYTE((val),1) + \
	GETBITBYTE((val),2) + \
	GETBITBYTE((val),3) + \
	GETBITBYTE((val),4) + \
	GETBITBYTE((val),5) + \
	GETBITBYTE((val),6) + \
	GETBITBYTE((val),7)   \
)

#define GETENTRY(vec,pos) ((GISTTYPE *) DatumGetPointer((vec)->vector[(pos)].key))

#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )

/* shorthand for calculating CRC-32 of a single chunk of data. */
static pg_crc32
crc32_sz(const char *buf, int size)
{
	pg_crc32	crc;

	INIT_TRADITIONAL_CRC32(crc);
	COMP_TRADITIONAL_CRC32(crc, buf, size);
	FIN_TRADITIONAL_CRC32(crc);

	return crc;
}


PG_FUNCTION_INFO_V1(ghstore_in);
PG_FUNCTION_INFO_V1(ghstore_out);


Datum
ghstore_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "ghstore")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

Datum
ghstore_out(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot display a value of type %s", "ghstore")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

static GISTTYPE *
ghstore_alloc(bool allistrue, int siglen, BITVECP sign)
{
	int			flag = allistrue ? ALLISTRUE : 0;
	int			size = CALCGTSIZE(flag, siglen);
	GISTTYPE   *res = palloc(size);

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

PG_FUNCTION_INFO_V1(ghstore_consistent);
PG_FUNCTION_INFO_V1(ghstore_compress);
PG_FUNCTION_INFO_V1(ghstore_decompress);
PG_FUNCTION_INFO_V1(ghstore_penalty);
PG_FUNCTION_INFO_V1(ghstore_picksplit);
PG_FUNCTION_INFO_V1(ghstore_union);
PG_FUNCTION_INFO_V1(ghstore_same);
PG_FUNCTION_INFO_V1(ghstore_options);

Datum
ghstore_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	int			siglen = GET_SIGLEN();
	GISTENTRY  *retval = entry;

	if (entry->leafkey)
	{
		GISTTYPE   *res = ghstore_alloc(false, siglen, NULL);
		HStore	   *val = DatumGetHStoreP(entry->key);
		HEntry	   *hsent = ARRPTR(val);
		char	   *ptr = STRPTR(val);
		int			count = HS_COUNT(val);
		int			i;

		for (i = 0; i < count; ++i)
		{
			int			h;

			h = crc32_sz((char *) HSTORE_KEY(hsent, ptr, i),
						 HSTORE_KEYLEN(hsent, i));
			HASH(GETSIGN(res), h, siglen);
			if (!HSTORE_VALISNULL(hsent, i))
			{
				h = crc32_sz((char *) HSTORE_VAL(hsent, ptr, i),
							 HSTORE_VALLEN(hsent, i));
				HASH(GETSIGN(res), h, siglen);
			}
		}

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset,
					  false);
	}
	else if (!ISALLTRUE(DatumGetPointer(entry->key)))
	{
		int32		i;
		GISTTYPE   *res;
		BITVECP		sign = GETSIGN(DatumGetPointer(entry->key));

		LOOPBYTE(siglen)
		{
			if ((sign[i] & 0xff) != 0xff)
				PG_RETURN_POINTER(retval);
		}

		res = ghstore_alloc(true, siglen, NULL);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset,
					  false);
	}

	PG_RETURN_POINTER(retval);
}

/*
 * Since type ghstore isn't toastable (and doesn't need to be),
 * this function can be a no-op.
 */
Datum
ghstore_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

Datum
ghstore_same(PG_FUNCTION_ARGS)
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

static int32
sizebitvec(BITVECP sign, int siglen)
{
	int32		size = 0,
				i;

	LOOPBYTE(siglen)
	{
		size += SUMBIT(sign);
		sign = (BITVECP) (((char *) sign) + 1);
	}
	return size;
}

static int
hemdistsign(BITVECP a, BITVECP b, int siglen)
{
	int			i,
				dist = 0;

	LOOPBIT(siglen)
	{
		if (GETBIT(a, i) != GETBIT(b, i))
			dist++;
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
ghstore_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int32		len = entryvec->n;

	int		   *size = (int *) PG_GETARG_POINTER(1);
	int			siglen = GET_SIGLEN();
	int32		i;
	GISTTYPE   *result = ghstore_alloc(false, siglen, NULL);
	BITVECP		base = GETSIGN(result);

	for (i = 0; i < len; i++)
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
ghstore_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0); /* always ISSIGNKEY */
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	int			siglen = GET_SIGLEN();
	GISTTYPE   *origval = (GISTTYPE *) DatumGetPointer(origentry->key);
	GISTTYPE   *newval = (GISTTYPE *) DatumGetPointer(newentry->key);

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
	return pg_cmp_s32(((const SPLITCOST *) a)->cost,
					  ((const SPLITCOST *) b)->cost);
}


Datum
ghstore_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	OffsetNumber maxoff = entryvec->n - 2;

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
	BITVECP		ptr;
	int			i;
	SPLITCOST  *costvector;
	GISTTYPE   *_k,
			   *_j;

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
	datum_l = ghstore_alloc(ISALLTRUE(GETENTRY(entryvec, seed_1)), siglen,
							GETSIGN(GETENTRY(entryvec, seed_1)));
	datum_r = ghstore_alloc(ISALLTRUE(GETENTRY(entryvec, seed_2)), siglen,
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
		costvector[j - 1].cost = abs(size_alpha - size_beta);
	}
	qsort(costvector, maxoff, sizeof(SPLITCOST), comparecost);

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

		if (size_alpha < size_beta + WISH_F(v->spl_nleft, v->spl_nright, 0.0001))
		{
			if (ISALLTRUE(datum_l) || ISALLTRUE(_j))
			{
				if (!ISALLTRUE(datum_l))
					memset(union_l, 0xff, siglen);
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
					memset(union_r, 0xff, siglen);
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

	v->spl_ldatum = PointerGetDatum(datum_l);
	v->spl_rdatum = PointerGetDatum(datum_r);

	PG_RETURN_POINTER(v);
}


Datum
ghstore_consistent(PG_FUNCTION_ARGS)
{
	GISTTYPE   *entry = (GISTTYPE *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	int			siglen = GET_SIGLEN();
	bool		res = true;
	BITVECP		sign;

	/* All cases served by this function are inexact */
	*recheck = true;

	if (ISALLTRUE(entry))
		PG_RETURN_BOOL(true);

	sign = GETSIGN(entry);

	if (strategy == HStoreContainsStrategyNumber ||
		strategy == HStoreOldContainsStrategyNumber)
	{
		HStore	   *query = PG_GETARG_HSTORE_P(1);
		HEntry	   *qe = ARRPTR(query);
		char	   *qv = STRPTR(query);
		int			count = HS_COUNT(query);
		int			i;

		for (i = 0; res && i < count; ++i)
		{
			int			crc = crc32_sz((char *) HSTORE_KEY(qe, qv, i),
									   HSTORE_KEYLEN(qe, i));

			if (GETBIT(sign, HASHVAL(crc, siglen)))
			{
				if (!HSTORE_VALISNULL(qe, i))
				{
					crc = crc32_sz((char *) HSTORE_VAL(qe, qv, i),
								   HSTORE_VALLEN(qe, i));
					if (!GETBIT(sign, HASHVAL(crc, siglen)))
						res = false;
				}
			}
			else
				res = false;
		}
	}
	else if (strategy == HStoreExistsStrategyNumber)
	{
		text	   *query = PG_GETARG_TEXT_PP(1);
		int			crc = crc32_sz(VARDATA_ANY(query), VARSIZE_ANY_EXHDR(query));

		res = (GETBIT(sign, HASHVAL(crc, siglen))) ? true : false;
	}
	else if (strategy == HStoreExistsAllStrategyNumber)
	{
		ArrayType  *query = PG_GETARG_ARRAYTYPE_P(1);
		Datum	   *key_datums;
		bool	   *key_nulls;
		int			key_count;
		int			i;

		deconstruct_array_builtin(query, TEXTOID, &key_datums, &key_nulls, &key_count);

		for (i = 0; res && i < key_count; ++i)
		{
			int			crc;

			if (key_nulls[i])
				continue;
			crc = crc32_sz(VARDATA(key_datums[i]), VARSIZE(key_datums[i]) - VARHDRSZ);
			if (!(GETBIT(sign, HASHVAL(crc, siglen))))
				res = false;
		}
	}
	else if (strategy == HStoreExistsAnyStrategyNumber)
	{
		ArrayType  *query = PG_GETARG_ARRAYTYPE_P(1);
		Datum	   *key_datums;
		bool	   *key_nulls;
		int			key_count;
		int			i;

		deconstruct_array_builtin(query, TEXTOID, &key_datums, &key_nulls, &key_count);

		res = false;

		for (i = 0; !res && i < key_count; ++i)
		{
			int			crc;

			if (key_nulls[i])
				continue;
			crc = crc32_sz(VARDATA(key_datums[i]), VARSIZE(key_datums[i]) - VARHDRSZ);
			if (GETBIT(sign, HASHVAL(crc, siglen)))
				res = true;
		}
	}
	else
		elog(ERROR, "Unsupported strategy number: %d", strategy);

	PG_RETURN_BOOL(res);
}

Datum
ghstore_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(GistHstoreOptions));
	add_local_int_reloption(relopts, "siglen",
							"signature length in bytes",
							SIGLEN_DEFAULT, 1, SIGLEN_MAX,
							offsetof(GistHstoreOptions, siglen));

	PG_RETURN_VOID();
}
