/******************************************************************************
  This file contains routines that can be bound to a Postgres backend and
  called by the backend in the process of processing queries.  The calling
  format for these routines is dictated by Postgres architecture.
******************************************************************************/

/*
#define GIST_DEBUG
#define GIST_QUERY_DEBUG
*/

#include "postgres.h"

#include <float.h>

#include "access/gist.h"
#include "access/itup.h"
#include "access/rtree.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "storage/bufpage.h"

/* number ranges for compression */
#define MAXNUMRANGE 100

#define max(a,b)		((a) >	(b) ? (a) : (b))
#define min(a,b)		((a) <= (b) ? (a) : (b))
#define abs(a)			((a) <	(0) ? -(a) : (a))

/* dimension of array */
#define NDIM 1

/* useful macros for accessing int4 arrays */
#define ARRPTR(x)  ( (int4 *) ARR_DATA_PTR(x) )
#define ARRNELEMS(x)  ArrayGetNItems( ARR_NDIM(x), ARR_DIMS(x))

#define ARRISNULL(x) ( (x) ? ( ( ARR_NDIM(x) == NDIM ) ? ( ( ARRNELEMS( x ) ) ? 0 : 1 ) : ( ( ARR_NDIM(x) ) ? (elog(ERROR,"Array is not one-dimensional: %d dimensions", ARR_NDIM(x)),1) : 1 ) ) : 1 )
#define ARRISVOID(x) ( (x) ? ( ( ARR_NDIM(x) == NDIM ) ? ( ( ARRNELEMS( x ) ) ? 0 : 1 ) : 1  ) : 0 )

#define SORT(x) \
	do { \
		 if ( ARRNELEMS( x ) > 1 ) \
			isort( ARRPTR( x ), ARRNELEMS( x ) ); \
	} while(0)

#define PREPAREARR(x) \
	do { \
		 if ( ARRNELEMS( x ) > 1 ) \
			if ( isort( ARRPTR( x ), ARRNELEMS( x ) ) ) \
				x = _int_unique( x ); \
	} while(0)

/* "wish" function */
#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )


/* bigint defines */
#define BITBYTE 8
#define SIGLENINT  64			/* >122 => key will toast, so very slow!!! */
#define SIGLEN	( sizeof(int)*SIGLENINT )
#define SIGLENBIT (SIGLEN*BITBYTE)

typedef char BITVEC[SIGLEN];
typedef char *BITVECP;

#define SIGPTR(x)  ( (BITVECP) ARR_DATA_PTR(x) )


#define LOOPBYTE(a) \
		for(i=0;i<SIGLEN;i++) {\
				a;\
		}

#define LOOPBIT(a) \
		for(i=0;i<SIGLENBIT;i++) {\
				a;\
		}

/* beware of multiple evaluation of arguments to these macros! */
#define GETBYTEBIT(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITBYTE ) ) )
#define CLRBIT(x,i)   GETBYTEBIT(x,i) &= ~( 0x01 << ( (i) % BITBYTE ) )
#define SETBIT(x,i)   GETBYTEBIT(x,i) |=  ( 0x01 << ( (i) % BITBYTE ) )
#define GETBIT(x,i) ( (GETBYTEBIT(x,i) >> ( (i) % BITBYTE )) & 0x01 )


#ifdef GIST_DEBUG
static void
printarr(ArrayType *a, int num)
{
	char		bbb[16384];
	char	   *cur;
	int			l;
	int		   *d;

	d = ARRPTR(a);
	*bbb = '\0';
	cur = bbb;
	for (l = 0; l < min(num, ARRNELEMS(a)); l++)
	{
		sprintf(cur, "%d ", d[l]);
		cur = strchr(cur, '\0');
	}
	elog(NOTICE, "\t\t%s", bbb);
}
static void
printbitvec(BITVEC bv)
{
	int			i;
	char		str[SIGLENBIT + 1];

	str[SIGLENBIT] = '\0';
	LOOPBIT(str[i] = (GETBIT(bv, i)) ? '1' : '0');

	elog(NOTICE, "BV: %s", str);
}

#endif

/*
** types for functions
*/
typedef ArrayType *(*formarray) (ArrayType *, ArrayType *);
typedef void (*formfloat) (ArrayType *, float *);

/*
** usefull function
*/
static bool isort(int4 *a, const int len);
static ArrayType *new_intArrayType(int num);
static ArrayType *copy_intArrayType(ArrayType *a);
static ArrayType *resize_intArrayType(ArrayType *a, int num);
static int	internal_size(int *a, int len);
static ArrayType *_int_unique(ArrayType *a);

/* common GiST function*/
static GIST_SPLITVEC *_int_common_picksplit(bytea *entryvec,
					  GIST_SPLITVEC *v,
					  formarray unionf,
					  formarray interf,
					  formfloat sizef,
					  float coef);
static float *_int_common_penalty(GISTENTRY *origentry,
					GISTENTRY *newentry,
					float *result,
					formarray unionf,
					formfloat sizef);
static ArrayType *_int_common_union(bytea *entryvec,
				  int *sizep,
				  formarray unionf);

/*
** GiST support methods
*/
PG_FUNCTION_INFO_V1( g_int_consistent );
PG_FUNCTION_INFO_V1( g_int_compress );
PG_FUNCTION_INFO_V1( g_int_decompress );
PG_FUNCTION_INFO_V1( g_int_penalty );
PG_FUNCTION_INFO_V1( g_int_picksplit );
PG_FUNCTION_INFO_V1( g_int_union );
PG_FUNCTION_INFO_V1( g_int_same );

Datum	g_int_consistent(PG_FUNCTION_ARGS);
Datum	g_int_compress(PG_FUNCTION_ARGS);
Datum	g_int_decompress(PG_FUNCTION_ARGS);
Datum	g_int_penalty(PG_FUNCTION_ARGS);
Datum	g_int_picksplit(PG_FUNCTION_ARGS);
Datum	g_int_union(PG_FUNCTION_ARGS);
Datum	g_int_same(PG_FUNCTION_ARGS);


/*
** R-tree support functions
*/
static bool		inner_int_contains(ArrayType *a, ArrayType *b);
static bool		inner_int_overlap(ArrayType *a, ArrayType *b);
static ArrayType  *inner_int_union(ArrayType *a, ArrayType *b);
static ArrayType  *inner_int_inter(ArrayType *a, ArrayType *b);
static void		rt__int_size(ArrayType *a, float *sz);

PG_FUNCTION_INFO_V1( _int_different );
PG_FUNCTION_INFO_V1( _int_same );
PG_FUNCTION_INFO_V1( _int_contains );
PG_FUNCTION_INFO_V1( _int_contained );
PG_FUNCTION_INFO_V1( _int_overlap );
PG_FUNCTION_INFO_V1( _int_union );
PG_FUNCTION_INFO_V1( _int_inter );

Datum	_int_different(PG_FUNCTION_ARGS);
Datum	_int_same(PG_FUNCTION_ARGS);
Datum	_int_contains(PG_FUNCTION_ARGS);
Datum	_int_contained(PG_FUNCTION_ARGS);
Datum	_int_overlap(PG_FUNCTION_ARGS);
Datum	_int_union(PG_FUNCTION_ARGS);
Datum	_int_inter(PG_FUNCTION_ARGS);

/*
** _intbig methods
*/
PG_FUNCTION_INFO_V1( g_intbig_consistent );
PG_FUNCTION_INFO_V1( g_intbig_compress );
PG_FUNCTION_INFO_V1( g_intbig_decompress );
PG_FUNCTION_INFO_V1( g_intbig_penalty );
PG_FUNCTION_INFO_V1( g_intbig_picksplit );
PG_FUNCTION_INFO_V1( g_intbig_union );
PG_FUNCTION_INFO_V1( g_intbig_same );

Datum	g_intbig_consistent(PG_FUNCTION_ARGS);
Datum	g_intbig_compress(PG_FUNCTION_ARGS);
Datum	g_intbig_decompress(PG_FUNCTION_ARGS);
Datum	g_intbig_penalty(PG_FUNCTION_ARGS);
Datum	g_intbig_picksplit(PG_FUNCTION_ARGS);
Datum	g_intbig_union(PG_FUNCTION_ARGS);
Datum	g_intbig_same(PG_FUNCTION_ARGS);

static bool _intbig_contains(ArrayType *a, ArrayType *b);
static bool _intbig_overlap(ArrayType *a, ArrayType *b);
static ArrayType *_intbig_union(ArrayType *a, ArrayType *b);

static ArrayType *	_intbig_inter(ArrayType *a, ArrayType *b);
static void rt__intbig_size(ArrayType *a, float *sz);
static void gensign(BITVEC sign, int *a, int len);

/*****************************************************************************
 *						   GiST functions
 *****************************************************************************/

/*
** The GiST Consistent method for _intments
** Should return false if for all data items x below entry,
** the predicate x op query == FALSE, where op is the oper
** corresponding to strategy in the pg_amop table.
*/
Datum
g_int_consistent(PG_FUNCTION_ARGS) {
	GISTENTRY *entry = (GISTENTRY *)PG_GETARG_POINTER(0);
	ArrayType *query = ( ArrayType * )PG_GETARG_POINTER(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool		retval;

	/* sort query for fast search, key is already sorted */
	/* XXX are we sure it's safe to scribble on the query object here? */
	/* XXX what about toasted input? */
	if (ARRISNULL(query))
		return FALSE;
	PREPAREARR(query);

	switch (strategy)
	{
		case RTOverlapStrategyNumber:
			retval = inner_int_overlap((ArrayType *) DatumGetPointer(entry->key),
									   query);
			break;
		case RTSameStrategyNumber:
		case RTContainsStrategyNumber:
			retval = inner_int_contains((ArrayType *) DatumGetPointer(entry->key),
										query);
			break;
		case RTContainedByStrategyNumber:
			retval = inner_int_overlap((ArrayType *) DatumGetPointer(entry->key),
									   query);
			break;
		default:
			retval = FALSE;
	}
	PG_RETURN_BOOL(retval);
}

Datum
g_int_union(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER( _int_common_union(
		(bytea *) PG_GETARG_POINTER(0), 
		(int *) PG_GETARG_POINTER(1), 
		inner_int_union 
	) );
}

/*
** GiST Compress and Decompress methods
*/
Datum
g_int_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY *entry = (GISTENTRY *)PG_GETARG_POINTER(0);
	GISTENTRY  *retval;
	ArrayType  *r;
	int			len;
	int		   *dr;
	int			i,
				min,
				cand;

	retval = palloc(sizeof(GISTENTRY));

	if (DatumGetPointer(entry->key) != NULL)
		r = (ArrayType *) PG_DETOAST_DATUM_COPY(entry->key);
	else
		r = NULL;

	if (ARRISNULL(r))
	{
		if ( ARRISVOID(r) ) {
			ArrayType *out = new_intArrayType( 0 );
			gistentryinit(*retval, PointerGetDatum(out),
				  entry->rel, entry->page, entry->offset, VARSIZE(out), FALSE);
		} else {
			gistentryinit(*retval, (Datum) 0, entry->rel, entry->page, entry->offset,
					  0, FALSE);
		}
		if (r) pfree(r);

		PG_RETURN_POINTER(retval);
	}

	if (entry->leafkey)
		PREPAREARR(r);
	len = ARRNELEMS(r);

#ifdef GIST_DEBUG
	elog(NOTICE, "COMP IN: %d leaf; %d rel; %d page; %d offset; %d bytes; %d elems", entry->leafkey, (int) entry->rel, (int) entry->page, (int) entry->offset, (int) entry->bytes, len);
#endif

	if (len >= 2 * MAXNUMRANGE)
	{							/* compress */
		r = resize_intArrayType(r, 2 * (len));

		dr = ARRPTR(r);

		for (i = len - 1; i >= 0; i--)
			dr[2 * i] = dr[2 * i + 1] = dr[i];

		len *= 2;
		cand = 1;
		while (len > MAXNUMRANGE * 2)
		{
			min = 0x7fffffff;
			for (i = 2; i < len; i += 2)
				if (min > (dr[i] - dr[i - 1]))
				{
					min = (dr[i] - dr[i - 1]);
					cand = i;
				}
			memmove((void *) &dr[cand - 1], (void *) &dr[cand + 1], (len - cand - 1) * sizeof(int));
			len -= 2;
		}
		r = resize_intArrayType(r, len);
	}

	gistentryinit(*retval, PointerGetDatum(r),
				  entry->rel, entry->page, entry->offset, VARSIZE(r), FALSE);

	PG_RETURN_POINTER(retval);
}

Datum
g_int_decompress(PG_FUNCTION_ARGS)
{
	GISTENTRY *entry = (GISTENTRY *)PG_GETARG_POINTER(0);
	GISTENTRY  *retval;
	ArrayType  *r;
	int		   *dr,
				lenr;
	ArrayType  *in;
	int			lenin;
	int		   *din;
	int			i,
				j;

	if (DatumGetPointer(entry->key) != NULL)
		in = (ArrayType *) PG_DETOAST_DATUM(entry->key);
	else
		in = NULL;

	if (ARRISNULL(in))
	{
		retval = palloc(sizeof(GISTENTRY));

		if ( ARRISVOID(in) ) {
			r = new_intArrayType( 0 );
			gistentryinit(*retval, PointerGetDatum(r),
				  entry->rel, entry->page, entry->offset, VARSIZE(r), FALSE);
		} else {
			gistentryinit(*retval, (Datum) 0, entry->rel, entry->page, entry->offset, 0, FALSE);
		}  
		if (in)
			if (in != (ArrayType *) DatumGetPointer(entry->key))
				pfree(in);
#ifdef GIST_DEBUG
		elog(NOTICE, "DECOMP IN: NULL");
#endif
		PG_RETURN_POINTER(retval);
	}


	lenin = ARRNELEMS(in);
	din = ARRPTR(in);

	if (lenin < 2 * MAXNUMRANGE)
	{							/* not comressed value */
		/* sometimes strange bytesize */
		gistentryinit(*entry, PointerGetDatum(in), entry->rel, entry->page, entry->offset, VARSIZE(in), FALSE);
		PG_RETURN_POINTER(entry);
	}

#ifdef GIST_DEBUG
	elog(NOTICE, "DECOMP IN: %d leaf; %d rel; %d page; %d offset; %d bytes; %d elems", entry->leafkey, (int) entry->rel, (int) entry->page, (int) entry->offset, (int) entry->bytes, lenin);
#endif

	lenr = internal_size(din, lenin);

	r = new_intArrayType(lenr);
	dr = ARRPTR(r);

	for (i = 0; i < lenin; i += 2)
		for (j = din[i]; j <= din[i + 1]; j++)
			if ((!i) || *(dr - 1) != j)
				*dr++ = j;

	if (in != (ArrayType *) DatumGetPointer(entry->key))
		pfree(in);
	retval = palloc(sizeof(GISTENTRY));

	gistentryinit(*retval, PointerGetDatum(r), entry->rel, entry->page, entry->offset, VARSIZE(r), FALSE);

	PG_RETURN_POINTER(retval);
}

/*
** The GiST Penalty method for _intments
*/
Datum
g_int_penalty(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER( _int_common_penalty( 
		(GISTENTRY *)PG_GETARG_POINTER(0), 
		(GISTENTRY *)PG_GETARG_POINTER(1), 
		(float *)    PG_GETARG_POINTER(2), 
		inner_int_union, rt__int_size
	) );
}


Datum
g_int_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER( _int_common_picksplit(
		(bytea *)PG_GETARG_POINTER(0), 
		(GIST_SPLITVEC *)PG_GETARG_POINTER(1),
		inner_int_union,
		inner_int_inter,
		rt__int_size,
		1e-8
	) );
}

/*
** Equality methods
*/


Datum
g_int_same(PG_FUNCTION_ARGS)
{
	bool *result = (bool *)PG_GETARG_POINTER(2);
	*result = DatumGetBool(
		DirectFunctionCall2(
			_int_same,
			PointerGetDatum(PG_GETARG_POINTER(0)),
			PointerGetDatum(PG_GETARG_POINTER(1))
		)
	);

	PG_RETURN_POINTER(result);
}

Datum 
_int_contained(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL( DatumGetBool( 
		DirectFunctionCall2( 
			_int_contains, 
			PointerGetDatum(PG_GETARG_POINTER(1)), 
			PointerGetDatum(PG_GETARG_POINTER(0)) 
		)
	));
}

Datum
_int_contains(PG_FUNCTION_ARGS)
{
	ArrayType *a = (ArrayType *)PG_GETARG_POINTER(0);
	ArrayType *b = (ArrayType *)PG_GETARG_POINTER(1);
	bool		res;
	ArrayType  *an,
			   *bn;

	if (ARRISNULL(a) || ARRISNULL(b))
		return FALSE;

	an = copy_intArrayType(a);
	bn = copy_intArrayType(b);

	PREPAREARR(an);
	PREPAREARR(bn);

	res = inner_int_contains(an, bn);
	pfree(an);
	pfree(bn);
	PG_RETURN_BOOL( res );
}

static bool
inner_int_contains(ArrayType *a, ArrayType *b)
{
	int			na,
				nb;
	int			i,
				j,
				n;
	int		   *da,
			   *db;

	if (ARRISNULL(a) || ARRISNULL(b))
		return FALSE;

	na = ARRNELEMS(a);
	nb = ARRNELEMS(b);
	da = ARRPTR(a);
	db = ARRPTR(b);

#ifdef GIST_DEBUG
	elog(NOTICE, "contains %d %d", na, nb);
#endif

	i = j = n = 0;
	while (i < na && j < nb)
		if (da[i] < db[j])
			i++;
		else if (da[i] == db[j])
		{
			n++;
			i++;
			j++;
		}
		else
			j++;

	return (n == nb) ? TRUE : FALSE;
}

/*****************************************************************************
 * Operator class for R-tree indexing
 *****************************************************************************/

Datum 
_int_different(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL( ! DatumGetBool( 
		DirectFunctionCall2( 
			_int_same, 
			PointerGetDatum(PG_GETARG_POINTER(0)), 
			PointerGetDatum(PG_GETARG_POINTER(1)) 
		)
	));
}

Datum
_int_same(PG_FUNCTION_ARGS)
{
	ArrayType *a = (ArrayType *)PG_GETARG_POINTER(0);
	ArrayType *b = (ArrayType *)PG_GETARG_POINTER(1);
	int			na,
				nb;
	int			n;
	int		   *da,
			   *db;
	bool		result;
	ArrayType  *an,
			   *bn;
	bool		anull = ARRISNULL(a);
	bool		bnull = ARRISNULL(b);

	if (anull || bnull)
		return (anull && bnull) ? TRUE : FALSE;

	an = copy_intArrayType(a);
	bn = copy_intArrayType(b);

	SORT(an);
	SORT(bn);
	na = ARRNELEMS(an);
	nb = ARRNELEMS(bn);
	da = ARRPTR(an);
	db = ARRPTR(bn);

	result = FALSE;

	if (na == nb)
	{
		result = TRUE;
		for (n = 0; n < na; n++)
			if (da[n] != db[n])
			{
				result = FALSE;
				break;
			}
	}

	pfree(an);
	pfree(bn);

	PG_RETURN_BOOL(result);
}

/*	_int_overlap -- does a overlap b?
 */
Datum
_int_overlap(PG_FUNCTION_ARGS)
{
	ArrayType *a = (ArrayType *)PG_GETARG_POINTER(0);
	ArrayType *b = (ArrayType *)PG_GETARG_POINTER(1);
	bool		result;
	ArrayType  *an,
			   *bn;

	if (ARRISNULL(a) || ARRISNULL(b))
		return FALSE;

	an = copy_intArrayType(a);
	bn = copy_intArrayType(b);

	SORT(an);
	SORT(bn);

	result = inner_int_overlap(an, bn);

	pfree(an);
	pfree(bn);

	PG_RETURN_BOOL( result );
}

static bool
inner_int_overlap(ArrayType *a, ArrayType *b)
{
	int			na,
				nb;
	int			i,
				j;
	int		   *da,
			   *db;

	if (ARRISNULL(a) || ARRISNULL(b))
		return FALSE;

	na = ARRNELEMS(a);
	nb = ARRNELEMS(b);
	da = ARRPTR(a);
	db = ARRPTR(b);

#ifdef GIST_DEBUG
	elog(NOTICE, "g_int_overlap");
#endif

	i = j = 0;
	while (i < na && j < nb)
		if (da[i] < db[j])
			i++;
		else if (da[i] == db[j])
			return TRUE;
		else
			j++;

	return FALSE;
}

Datum
_int_union(PG_FUNCTION_ARGS)
{
	ArrayType *a = (ArrayType *)PG_GETARG_POINTER(0);
	ArrayType *b = (ArrayType *)PG_GETARG_POINTER(1);
	ArrayType  *result;
	ArrayType  *an,
			   *bn;

	an = copy_intArrayType(a);
	bn = copy_intArrayType(b);

	if (!ARRISNULL(an))
		SORT(an);
	if (!ARRISNULL(bn))
		SORT(bn);

	result = inner_int_union(an, bn);

	if (an)
		pfree(an);
	if (bn)
		pfree(bn);

	PG_RETURN_POINTER( result );
}

static ArrayType  *
inner_int_union(ArrayType *a, ArrayType *b)
{
	ArrayType  *r = NULL;
	int			na,
				nb;
	int		   *da,
			   *db,
			   *dr;
	int			i,
				j;

#ifdef GIST_DEBUG
	elog(NOTICE, "inner_union %d %d", ARRISNULL(a), ARRISNULL(b));
#endif

	if (ARRISNULL(a) && ARRISNULL(b))
		return new_intArrayType(0);
	if (ARRISNULL(a))
		r = copy_intArrayType(b);
	if (ARRISNULL(b))
		r = copy_intArrayType(a);

	if (r)
		dr = ARRPTR(r);
	else
	{
		na = ARRNELEMS(a);
		nb = ARRNELEMS(b);
		da = ARRPTR(a);
		db = ARRPTR(b);

		r = new_intArrayType(na + nb);
		dr = ARRPTR(r);

		/* union */
		i = j = 0;
		while (i < na && j < nb)
			if (da[i] < db[j])
				*dr++ = da[i++];
			else
				*dr++ = db[j++];

		while (i < na)
			*dr++ = da[i++];
		while (j < nb)
			*dr++ = db[j++];

	}

	if (ARRNELEMS(r) > 1)
		r = _int_unique(r);

	return r;
}


Datum
_int_inter(PG_FUNCTION_ARGS)
{
	ArrayType *a = (ArrayType *)PG_GETARG_POINTER(0);
	ArrayType *b = (ArrayType *)PG_GETARG_POINTER(1);
	ArrayType  *result;
	ArrayType  *an,
			   *bn;

	if (ARRISNULL(a) || ARRISNULL(b))
		PG_RETURN_POINTER(new_intArrayType(0));

	an = copy_intArrayType(a);
	bn = copy_intArrayType(b);

	SORT(an);
	SORT(bn);

	result = inner_int_inter(an, bn);

	pfree(an);
	pfree(bn);

	PG_RETURN_POINTER( result );
}

static ArrayType  *
inner_int_inter(ArrayType *a, ArrayType *b)
{
	ArrayType  *r;
	int			na,
				nb;
	int		   *da,
			   *db,
			   *dr;
	int			i,
				j;

#ifdef GIST_DEBUG
	elog(NOTICE, "inner_inter %d %d", ARRISNULL(a), ARRISNULL(b));
#endif

	if (ARRISNULL(a) || ARRISNULL(b))
		return new_intArrayType(0);

	na = ARRNELEMS(a);
	nb = ARRNELEMS(b);
	da = ARRPTR(a);
	db = ARRPTR(b);
	r = new_intArrayType(min(na, nb));
	dr = ARRPTR(r);

	i = j = 0;
	while (i < na && j < nb)
		if (da[i] < db[j])
			i++;
		else if (da[i] == db[j])
		{
			if (i + j == 0 || (i + j > 0 && *(dr - 1) != db[j]))
				*dr++ = db[j];
			i++;
			j++;
		}
		else
			j++;

	if ((dr - ARRPTR(r)) == 0)
	{
		pfree(r);
		return new_intArrayType(0);
	}
	else
		return resize_intArrayType(r, dr - ARRPTR(r));
}

static void
rt__int_size(ArrayType *a, float *size)
{
	if (ARRISNULL(a))
		*size = 0.0;
	else
		*size = (float) ARRNELEMS(a);

	return;
}


/*****************************************************************************
 *				   Miscellaneous operators and functions
 *****************************************************************************/

/* len >= 2 */
static bool
isort(int4 *a, int len)
{
	int4			tmp,
				index;
	int4		   *cur,
			   *end;
	bool		r = FALSE;

	end = a + len;
	do
	{
		index = 0;
		cur = a + 1;
		while (cur < end)
		{
			if (*(cur - 1) > *cur)
			{
				tmp = *(cur - 1);
				*(cur - 1) = *cur;
				*cur = tmp;
				index = 1;
			}
			else if (!r && *(cur - 1) == *cur)
				r = TRUE;
			cur++;
		}
	} while (index);
	return r;
}

static ArrayType *
new_intArrayType(int num)
{
	ArrayType  *r;
	int			nbytes = ARR_OVERHEAD(NDIM) + sizeof(int) * num;

	r = (ArrayType *) palloc(nbytes);

	MemSet(r, 0, nbytes);
	r->size = nbytes;
	r->ndim = NDIM;
	*((int *) ARR_DIMS(r)) = num;
	*((int *) ARR_LBOUND(r)) = 1;

	return r;
}

static ArrayType *
resize_intArrayType(ArrayType *a, int num)
{
	int			nbytes = ARR_OVERHEAD(NDIM) + sizeof(int) * num;

	if (num == ARRNELEMS(a))
		return a;

	a = (ArrayType *) repalloc(a, nbytes);

	a->size = nbytes;
	*((int *) ARR_DIMS(a)) = num;
	return a;
}

static ArrayType *
copy_intArrayType(ArrayType *a)
{
	ArrayType  *r;

	if (ARRISNULL(a))
		return NULL;
	r = new_intArrayType(ARRNELEMS(a));
	memmove(r, a, VARSIZE(a));
	return r;
}

/* num for compressed key */
static int
internal_size(int *a, int len)
{
	int			i,
				size = 0;

	for (i = 0; i < len; i += 2)
		if (!i || a[i] != a[i - 1])		/* do not count repeated range */
			size += a[i + 1] - a[i] + 1;

	return size;
}

/* r is sorted and size of r > 1 */
static ArrayType *
_int_unique(ArrayType *r)
{
	int		   *tmp,
			   *dr,
			   *data;
	int			num = ARRNELEMS(r);

	data = tmp = dr = ARRPTR(r);
	while (tmp - data < num)
		if (*tmp != *dr)
			*(++dr) = *tmp++;
		else
			tmp++;
	return resize_intArrayType(r, dr + 1 - ARRPTR(r));
}

/*********************************************************************
** intbig functions
*********************************************************************/
static void
gensign(BITVEC sign, int *a, int len)
{
	int			i;

	/* we assume that the sign vector is previously zeroed */
	for (i = 0; i < len; i++)
	{
		SETBIT(sign, (*a) % SIGLENBIT);
		a++;
	}
}

static bool
_intbig_overlap(ArrayType *a, ArrayType *b)
{
	int			i;
	BITVECP		da,
				db;

	if (ARRISNULL(a) || ARRISNULL(b))
		return FALSE;
	da = SIGPTR(a);
	db = SIGPTR(b);

	LOOPBYTE(if (da[i] & db[i]) return TRUE);
	return FALSE;
}

static bool
_intbig_contains(ArrayType *a, ArrayType *b)
{
	int			i;
	BITVECP		da,
				db;

	if (ARRISNULL(a) || ARRISNULL(b))
		return FALSE;
	da = SIGPTR(a);
	db = SIGPTR(b);

	LOOPBYTE(if (db[i] & ~da[i]) return FALSE);

	return TRUE;
}

static void
rt__intbig_size(ArrayType *a, float *sz)
{
	int			i,
				len = 0;
	BITVECP		bv;

	if (ARRISNULL(a))
	{
		*sz = 0.0;
		return;
	}

	bv = SIGPTR(a);
	LOOPBIT(len += GETBIT(bv, i));
	*sz = (float) len;
	return;
}

static ArrayType *
_intbig_union(ArrayType *a, ArrayType *b)
{
	ArrayType  *r;
	BITVECP		da,
				db,
				dr;
	int			i;

	if (ARRISNULL(a) && ARRISNULL(b))
		return new_intArrayType(0);
	if (ARRISNULL(a))
		return copy_intArrayType(b);
	if (ARRISNULL(b))
		return copy_intArrayType(a);

	r = new_intArrayType(SIGLENINT);

	da = SIGPTR(a);
	db = SIGPTR(b);
	dr = SIGPTR(r);

	LOOPBYTE(dr[i] = da[i] | db[i]);

	return r;
}

static ArrayType *
_intbig_inter(ArrayType *a, ArrayType *b)
{
	ArrayType  *r;
	BITVECP		da,
				db,
				dr;
	int			i;

	if (ARRISNULL(a) || ARRISNULL(b))
		return new_intArrayType(0);

	r = new_intArrayType(SIGLENINT);

	da = SIGPTR(a);
	db = SIGPTR(b);
	dr = SIGPTR(r);

	LOOPBYTE(dr[i] = da[i] & db[i]);

	return r;
}

Datum
g_intbig_same(PG_FUNCTION_ARGS)
{
	ArrayType *a = (ArrayType *)PG_GETARG_POINTER(0);
	ArrayType *b = (ArrayType *)PG_GETARG_POINTER(1);
	bool *result = (bool *)PG_GETARG_POINTER(2);
	BITVECP		da,
				db;
	int			i;

	if (ARRISNULL(a) || ARRISNULL(b))
	{
		*result = (ARRISNULL(a) && ARRISNULL(b)) ? TRUE : FALSE;
		PG_RETURN_POINTER( result );
	}

	da = SIGPTR(a);
	db = SIGPTR(b);

	LOOPBYTE(
	if (da[i] != db[i])
	{
		*result = FALSE;
		PG_RETURN_POINTER( result );
	}
	);

	*result = TRUE;
	PG_RETURN_POINTER( result );
}

Datum
g_intbig_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY *entry = (GISTENTRY *)PG_GETARG_POINTER(0);
	GISTENTRY  *retval;
	ArrayType  *r,
			   *in;
	bool maycompress = true;
	int i;

	if (DatumGetPointer(entry->key) != NULL)
		in = (ArrayType *) PG_DETOAST_DATUM(entry->key);
	else
		in = NULL;

	if (!entry->leafkey) {
		if ( ! ARRISNULL(in) ) {
			LOOPBYTE( 
				if ( ( ((char*)ARRPTR(in))[i] & 0xff ) != 0xff ) {
					maycompress = false;
					break;
				}
			); 
			if ( maycompress ) {
				retval = palloc(sizeof(GISTENTRY));
				r = new_intArrayType(1);
				gistentryinit(*retval, PointerGetDatum(r), entry->rel, entry->page, entry->offset, VARSIZE(r), FALSE);
				PG_RETURN_POINTER( retval );
			}	
		}
		PG_RETURN_POINTER( entry );
	}

	retval = palloc(sizeof(GISTENTRY));

	if (ARRISNULL(in))
	{
		if ( ARRISVOID(in) ) {
			r = new_intArrayType( SIGLENINT );
			gistentryinit(*retval, PointerGetDatum(r),
				  entry->rel, entry->page, entry->offset, VARSIZE(r), FALSE);
		} else {
			gistentryinit(*retval, (Datum) 0, entry->rel, entry->page, entry->offset,
					  0, FALSE);
		}
		if (in)
			if (in != (ArrayType *) DatumGetPointer(entry->key))
				pfree(in);
		PG_RETURN_POINTER (retval);
	}

	r = new_intArrayType(SIGLENINT);
	gensign(SIGPTR(r),
			ARRPTR(in),
			ARRNELEMS(in));

	LOOPBYTE( 
		if( ( ((char*)ARRPTR(in))[i] & 0xff ) != 0xff ) {
			maycompress = false;
			break;
		}
	); 

	if ( maycompress ) {
		pfree(r);
		r = new_intArrayType(1);
	}	

	gistentryinit(*retval, PointerGetDatum(r), entry->rel, entry->page, entry->offset, VARSIZE(r), FALSE);

	if (in)
		if ( in != (ArrayType *) DatumGetPointer(entry->key))
			pfree(in);

	PG_RETURN_POINTER (retval);
}

Datum
g_intbig_decompress(PG_FUNCTION_ARGS)
{
	GISTENTRY *entry = (GISTENTRY *)PG_GETARG_POINTER(0);
	ArrayType  *key;

	if ( DatumGetPointer(entry->key) != NULL )
		key = (ArrayType *) PG_DETOAST_DATUM(entry->key);
	else
		key = NULL;

	if ( key != (ArrayType *) DatumGetPointer(entry->key))
	{
		GISTENTRY  *retval;

		retval = palloc(sizeof(GISTENTRY));

		gistentryinit(*retval, PointerGetDatum(key), entry->rel, entry->page, entry->offset, (key) ? VARSIZE(key) : 0, FALSE);
		PG_RETURN_POINTER( retval );
	}
	if ( ! ARRISNULL(key) )
		if ( ARRNELEMS(key) == 1 ) {
			GISTENTRY  *retval;
			ArrayType  *newkey;

			retval = palloc(sizeof(GISTENTRY));
			newkey = new_intArrayType(SIGLENINT);
			MemSet( (void*)ARRPTR(newkey), 0xff, SIGLEN ); 

			gistentryinit(*retval, PointerGetDatum(newkey), entry->rel, entry->page, entry->offset, VARSIZE(newkey), FALSE);
			PG_RETURN_POINTER( retval );
		}
	PG_RETURN_POINTER( entry );
}

Datum
g_intbig_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER( _int_common_picksplit(
		(bytea *)PG_GETARG_POINTER(0), 
		(GIST_SPLITVEC *)PG_GETARG_POINTER(1),
		_intbig_union,
		_intbig_inter,
		rt__intbig_size,
		1.0
	) );
}

Datum
g_intbig_union(PG_FUNCTION_ARGS)
{
        PG_RETURN_POINTER( _int_common_union(
                (bytea *) PG_GETARG_POINTER(0),
                (int *) PG_GETARG_POINTER(1),
                _intbig_union
        ) );
}

Datum
g_intbig_penalty(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER( _int_common_penalty( 
		(GISTENTRY *)PG_GETARG_POINTER(0), 
		(GISTENTRY *)PG_GETARG_POINTER(1), 
		(float *)    PG_GETARG_POINTER(2), 
		_intbig_union, rt__intbig_size
	) );
}

Datum
g_intbig_consistent(PG_FUNCTION_ARGS) {
        GISTENTRY *entry = (GISTENTRY *)PG_GETARG_POINTER(0);
        ArrayType *query = ( ArrayType * )PG_GETARG_POINTER(1);
        StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool		retval;
	ArrayType  *q;

	/* XXX what about toasted input? */
	if (ARRISNULL(query))
		return FALSE;

	q = new_intArrayType(SIGLENINT);
	gensign(SIGPTR(q),
			ARRPTR(query),
			ARRNELEMS(query));

	switch (strategy)
	{
		case RTOverlapStrategyNumber:
			retval = _intbig_overlap((ArrayType *) DatumGetPointer(entry->key), q);
			break;
		case RTSameStrategyNumber:
		case RTContainsStrategyNumber:
			retval = _intbig_contains((ArrayType *) DatumGetPointer(entry->key), q);
			break;
		case RTContainedByStrategyNumber:
			retval = _intbig_overlap((ArrayType *) DatumGetPointer(entry->key), q);
			break;
		default:
			retval = FALSE;
	}
	pfree(q);
	PG_RETURN_BOOL(retval);
}

/*****************************************************************
** Common GiST Method
*****************************************************************/

/*
** The GiST Union method for _intments
** returns the minimal set that encloses all the entries in entryvec
*/
static ArrayType  *
_int_common_union(bytea *entryvec, int *sizep, formarray unionf)
{
	int			numranges,
				i;
	ArrayType  *out = (ArrayType *) NULL;
	ArrayType  *tmp;

#ifdef GIST_DEBUG
	elog(NOTICE, "_int_common_union in");
#endif

	numranges = (VARSIZE(entryvec) - VARHDRSZ) / sizeof(GISTENTRY);
	tmp = (ArrayType *) DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[0].key);

	for (i = 1; i < numranges; i++)
	{
		out = (*unionf) (tmp, (ArrayType *)
						 DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[i].key));
		if (i > 1 && tmp)
			pfree(tmp);
		tmp = out;
	}

	*sizep = VARSIZE(out);
	if (*sizep == 0)
	{
		pfree(out);
#ifdef GIST_DEBUG
		elog(NOTICE, "_int_common_union out1");
#endif
		return NULL;
	}
#ifdef GIST_DEBUG
	elog(NOTICE, "_int_common_union out");
#endif
	return (out);

}

/*****************************************
 * The GiST Penalty method for _intments *
 *****************************************/

static float *
_int_common_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result,
					formarray unionf,
					formfloat sizef)
{
	ArrayType  *ud;
	float		tmp1,
				tmp2;

#ifdef GIST_DEBUG
	elog(NOTICE, "penalty");
#endif
	ud = (*unionf) ((ArrayType *) DatumGetPointer(origentry->key),
					(ArrayType *) DatumGetPointer(newentry->key));
	(*sizef) (ud, &tmp1);
	(*sizef) ((ArrayType *) DatumGetPointer(origentry->key), &tmp2);
	*result = tmp1 - tmp2;
	pfree(ud);

#ifdef GIST_DEBUG
	elog(NOTICE, "--penalty\t%g", *result);
#endif

	return (result);
}

/*
** The GiST PickSplit method for _intments
** We use Guttman's poly time split algorithm
*/
static GIST_SPLITVEC *
_int_common_picksplit(bytea *entryvec,
					  GIST_SPLITVEC *v,
					  formarray unionf,
					  formarray interf,
					  formfloat sizef,
					  float coef)
{
	OffsetNumber i,
				j;
	ArrayType  *datum_alpha,
			   *datum_beta;
	ArrayType  *datum_l,
			   *datum_r;
	ArrayType  *union_d,
			   *union_dl,
			   *union_dr;
	ArrayType  *inter_d;
	bool		firsttime;
	float		size_alpha,
				size_beta,
				size_union,
				size_inter;
	float		size_waste,
				waste;
	float		size_l,
				size_r;
	int			nbytes;
	OffsetNumber seed_1 = 0,
				seed_2 = 0;
	OffsetNumber *left,
			   *right;
	OffsetNumber maxoff;

#ifdef GIST_DEBUG
	elog(NOTICE, "--------picksplit %d", (VARSIZE(entryvec) - VARHDRSZ) / sizeof(GISTENTRY));
#endif

	maxoff = ((VARSIZE(entryvec) - VARHDRSZ) / sizeof(GISTENTRY)) - 2;
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	firsttime = true;
	waste = 0.0;

	for (i = FirstOffsetNumber; i < maxoff; i = OffsetNumberNext(i))
	{
		datum_alpha = (ArrayType *) DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[i].key);
		for (j = OffsetNumberNext(i); j <= maxoff; j = OffsetNumberNext(j))
		{
			datum_beta = (ArrayType *) DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[j].key);

			/* compute the wasted space by unioning these guys */
			/* size_waste = size_union - size_inter; */
			union_d = (*unionf) (datum_alpha, datum_beta);
			(*sizef) (union_d, &size_union);
			inter_d = (*interf) (datum_alpha, datum_beta);
			(*sizef) (inter_d, &size_inter);
			size_waste = size_union - size_inter;

			pfree(union_d);

			if (inter_d != (ArrayType *) NULL)
				pfree(inter_d);

			/*
			 * are these a more promising split that what we've already
			 * seen?
			 */

			if (size_waste > waste || firsttime)
			{
				waste = size_waste;
				seed_1 = i;
				seed_2 = j;
				firsttime = false;
			}
		}
	}

	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	datum_alpha = (ArrayType *) DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[seed_1].key);
	datum_l = copy_intArrayType(datum_alpha);
	(*sizef) (datum_l, &size_l);
	datum_beta = (ArrayType *) DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[seed_2].key);
	datum_r = copy_intArrayType(datum_beta);
	(*sizef) (datum_r, &size_r);

	/*
	 * Now split up the regions between the two seeds.	An important
	 * property of this split algorithm is that the split vector v has the
	 * indices of items to be split in order in its left and right
	 * vectors.  We exploit this property by doing a merge in the code
	 * that actually splits the page.
	 *
	 * For efficiency, we also place the new index tuple in this loop. This
	 * is handled at the very end, when we have placed all the existing
	 * tuples and i == maxoff + 1.
	 */

	maxoff = OffsetNumberNext(maxoff);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{


		/*
		 * If we've already decided where to place this item, just put it
		 * on the right list.  Otherwise, we need to figure out which page
		 * needs the least enlargement in order to store the item.
		 */

		if (i == seed_1)
		{
			*left++ = i;
			v->spl_nleft++;
			continue;
		}
		else if (i == seed_2)
		{
			*right++ = i;
			v->spl_nright++;
			continue;
		}

		/* okay, which page needs least enlargement? */
		datum_alpha = (ArrayType *) DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[i].key);
		union_dl = (*unionf) (datum_l, datum_alpha);
		union_dr = (*unionf) (datum_r, datum_alpha);
		(*sizef) (union_dl, &size_alpha);
		(*sizef) (union_dr, &size_beta);

		/* pick which page to add it to */
		if (size_alpha - size_l < size_beta - size_r + WISH_F(v->spl_nleft, v->spl_nright, coef))
		{
			if (datum_l)
				pfree(datum_l);
			if (union_dr)
				pfree(union_dr);
			datum_l = union_dl;
			size_l = size_alpha;
			*left++ = i;
			v->spl_nleft++;
		}
		else
		{
			if (datum_r)
				pfree(datum_r);
			if (union_dl)
				pfree(union_dl);
			datum_r = union_dr;
			size_r = size_beta;
			*right++ = i;
			v->spl_nright++;
		}
	}

	if (*(left - 1) > *(right - 1))
	{
		*right = FirstOffsetNumber;
		*(left - 1) = InvalidOffsetNumber;
	}
	else
	{
		*left = FirstOffsetNumber;
		*(right - 1) = InvalidOffsetNumber;
	}

	v->spl_ldatum = PointerGetDatum(datum_l);
	v->spl_rdatum = PointerGetDatum(datum_r);

#ifdef GIST_DEBUG
	elog(NOTICE, "--------ENDpicksplit %d %d", v->spl_nleft, v->spl_nright);
#endif
	return v;
}
