/******************************************************************************
  This file contains routines that can be bound to a Postgres backend and
  called by the backend in the process of processing queries.  The calling
  format for these routines is dictated by Postgres architecture.
******************************************************************************/

/*
#define BS_DEBUG
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

/* 
 * flags for gist__int_ops, use ArrayType->flags 
 * which is unused (see array.h)
 */
#define LEAFKEY		(1<<31)
#define ISLEAFKEY(x)	( ((ArrayType*)(x))->flags & LEAFKEY )

/* useful macros for accessing int4 arrays */
#define ARRPTR(x)  ( (int4 *) ARR_DATA_PTR(x) )
#define ARRNELEMS(x)  ArrayGetNItems( ARR_NDIM(x), ARR_DIMS(x))

#define ARRISVOID(x) ( (x) ? ( ( ARR_NDIM(x) == NDIM ) ? ( ( ARRNELEMS( x ) ) ? 0 : 1 ) : ( ( ARR_NDIM(x) ) ? (elog(ERROR,"Array is not one-dimensional: %d dimensions",ARRNELEMS( x )),1) : 0 )  ) : 0 )

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
#define GETBYTE(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITBYTE ) ) )
#define GETBITBYTE(x,i) ( (*((char*)(x)) >> (i)) & 0x01 )
#define CLRBIT(x,i)   GETBYTE(x,i) &= ~( 0x01 << ( (i) % BITBYTE ) )
#define SETBIT(x,i)   GETBYTE(x,i) |=  ( 0x01 << ( (i) % BITBYTE ) )
#define GETBIT(x,i) ( (GETBYTE(x,i) >> ( (i) % BITBYTE )) & 0x01 )
#define HASHVAL(val) (((unsigned int)(val)) % SIGLENBIT)
#define HASH(sign, val) SETBIT((sign), HASHVAL(val))


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



/*****************************************************************************
 *		 	Boolean Search	
 *****************************************************************************/

#define BooleanSearchStrategy	20

/*
 * item in polish notation with back link
 * to left operand
 */
typedef struct ITEM {
	int2    type;
	int2	left;
	int4    val;
} ITEM;

typedef struct {
	int4	len;
	int4	size;
	char	data[1];
} QUERYTYPE;

#define HDRSIZEQT	( 2*sizeof(int4) )
#define COMPUTESIZE(size)	( HDRSIZEQT + size * sizeof(ITEM) )
#define GETQUERY(x)  (ITEM*)( (char*)(x)+HDRSIZEQT )

PG_FUNCTION_INFO_V1(bqarr_in);
PG_FUNCTION_INFO_V1(bqarr_out);
Datum   bqarr_in(PG_FUNCTION_ARGS);  
Datum   bqarr_out(PG_FUNCTION_ARGS);
 
PG_FUNCTION_INFO_V1(boolop);
Datum   boolop(PG_FUNCTION_ARGS);  

PG_FUNCTION_INFO_V1(rboolop);
Datum   rboolop(PG_FUNCTION_ARGS);  

PG_FUNCTION_INFO_V1(querytree);
Datum   querytree(PG_FUNCTION_ARGS);  

static bool signconsistent( QUERYTYPE *query, BITVEC sign, bool leaf );
static bool execconsistent( QUERYTYPE *query, ArrayType *array, bool leaf );

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

	if ( strategy == BooleanSearchStrategy )
		PG_RETURN_BOOL(execconsistent( (QUERYTYPE*)query, 
						(ArrayType *) DatumGetPointer(entry->key),
						ISLEAFKEY( (ArrayType *) DatumGetPointer(entry->key) ) ) );
	
	/* XXX are we sure it's safe to scribble on the query object here? */
	/* XXX what about toasted input? */
	/* sort query for fast search, key is already sorted */
	if ( ARRISVOID( query ) )
		PG_RETURN_BOOL(false);
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
			if ( GIST_LEAF(entry) ) 
				retval = inner_int_contains(query,
					(ArrayType *) DatumGetPointer(entry->key) );
			else 
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

	if (entry->leafkey) {
		r = (ArrayType *) PG_DETOAST_DATUM_COPY(entry->key);
		PREPAREARR(r);
		r->flags |= LEAFKEY;
		retval = palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(r),
				  entry->rel, entry->page, entry->offset, VARSIZE(r), FALSE);

		PG_RETURN_POINTER(retval);
	}

	r = (ArrayType *) PG_DETOAST_DATUM(entry->key);
	if ( ISLEAFKEY( r ) || ARRISVOID(r) ) {
		if ( r != (ArrayType*)DatumGetPointer(entry->key) )
			pfree(r);
		PG_RETURN_POINTER(entry);
	} 

	if ( (len=ARRNELEMS(r)) >= 2 * MAXNUMRANGE) {	/* compress */
		if ( r == (ArrayType*)DatumGetPointer( entry->key) ) 
			r = (ArrayType *) PG_DETOAST_DATUM_COPY(entry->key);
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
		retval = palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(r),
				  entry->rel, entry->page, entry->offset, VARSIZE(r), FALSE);
		PG_RETURN_POINTER(retval);
	} else {
		PG_RETURN_POINTER(entry);
	}

	PG_RETURN_POINTER(entry);
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

	in = (ArrayType *) PG_DETOAST_DATUM(entry->key);

	if ( ARRISVOID(in) ) {
		PG_RETURN_POINTER(entry);
	}

	lenin = ARRNELEMS(in);

	if (lenin < 2 * MAXNUMRANGE || ISLEAFKEY( in ) ) { /* not comressed value */
		if ( in != (ArrayType *) DatumGetPointer(entry->key)) {
			retval = palloc(sizeof(GISTENTRY));
			gistentryinit(*retval, PointerGetDatum(in), 
				entry->rel, entry->page, entry->offset, VARSIZE(in), FALSE);

			PG_RETURN_POINTER(retval);
		} 
		PG_RETURN_POINTER(entry);
	}

	din = ARRPTR(in);
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
	gistentryinit(*retval, PointerGetDatum(r), 
		entry->rel, entry->page, entry->offset, VARSIZE(r), FALSE);

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
		0.01	
	) );
}

/*
** Equality methods
*/


Datum
g_int_same(PG_FUNCTION_ARGS)
{
	ArrayType	*a = (ArrayType*)PointerGetDatum(PG_GETARG_POINTER(0));
	ArrayType	*b = (ArrayType*)PointerGetDatum(PG_GETARG_POINTER(1));
	bool *result = (bool *)PG_GETARG_POINTER(2);
	int4 n = ARRNELEMS(a);
	int4 *da, *db;

	if ( n != ARRNELEMS(b) ) {
		*result = false;
		PG_RETURN_POINTER(result);
	}
	*result = TRUE;
	da = ARRPTR(a);
	db = ARRPTR(b);
	while(n--)
		if (*da++ != *db++) {
			*result = FALSE;
			break;
		}

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
	ArrayType *a = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));
	ArrayType *b = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(1)));
	bool		res;

	if (ARRISVOID(a) || ARRISVOID(b))
		return FALSE;

	PREPAREARR(a);
	PREPAREARR(b);
	res = inner_int_contains(a, b);
	pfree(a);
	pfree(b);
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

	if (ARRISVOID(a) || ARRISVOID(b))
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
	ArrayType *a = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));
	ArrayType *b = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(1)));
	int			na,
				nb;
	int			n;
	int		   *da,
			   *db;
	bool		result;
	bool		avoid = ARRISVOID(a);
	bool		bvoid = ARRISVOID(b);

	if (avoid || bvoid)
		return (avoid && bvoid) ? TRUE : FALSE;

	SORT(a);
	SORT(b);
	na = ARRNELEMS(a);
	nb = ARRNELEMS(b);
	da = ARRPTR(a);
	db = ARRPTR(b);

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

	pfree(a);
	pfree(b);

	PG_RETURN_BOOL(result);
}

/*	_int_overlap -- does a overlap b?
 */
Datum
_int_overlap(PG_FUNCTION_ARGS)
{
	ArrayType *a = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));
	ArrayType *b = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(1)));
	bool		result;

	if (ARRISVOID(a) || ARRISVOID(b))
		return FALSE;

	SORT(a);
	SORT(b);

	result = inner_int_overlap(a, b);

	pfree(a);
	pfree(b);

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

	if (ARRISVOID(a) || ARRISVOID(b))
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
	ArrayType *a = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));
	ArrayType *b = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(1)));
	ArrayType  *result;

	if (!ARRISVOID(a))
		SORT(a);
	if (!ARRISVOID(b))
		SORT(b);

	result = inner_int_union(a, b);

	if (a)
		pfree(a);
	if (b)
		pfree(b);

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

	if (ARRISVOID(a) && ARRISVOID(b))
		return new_intArrayType(0);
	if (ARRISVOID(a))
		r = copy_intArrayType(b);
	if (ARRISVOID(b))
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
	ArrayType *a = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0)));
	ArrayType *b = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(1)));
	ArrayType  *result;

	if (ARRISVOID(a) || ARRISVOID(b))
		PG_RETURN_POINTER(new_intArrayType(0));

	SORT(a);
	SORT(b);

	result = inner_int_inter(a, b);

	pfree(a);
	pfree(b);

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

	if (ARRISVOID(a) || ARRISVOID(b))
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
	r->flags &= ~LEAFKEY;
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
		HASH(sign, *a);
		a++;
	}
}

static bool
_intbig_overlap(ArrayType *a, ArrayType *b)
{
	int			i;
	BITVECP		da,
				db;

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
	BITVECP		bv = SIGPTR(a);

	LOOPBYTE(
		len +=
			GETBITBYTE(bv,0) +
			GETBITBYTE(bv,1) +
			GETBITBYTE(bv,2) +
			GETBITBYTE(bv,3) +
			GETBITBYTE(bv,4) +
			GETBITBYTE(bv,5) +
			GETBITBYTE(bv,6) +
			GETBITBYTE(bv,7) ;
		bv = (BITVECP) ( ((char*)bv) + 1 );
	);
	
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
		LOOPBYTE( 
			if ( ( ((char*)ARRPTR(in))[i] & 0xff ) != 0xff ) {
				maycompress = false;
				break;
			}
		); 
		if ( maycompress ) {
			retval = palloc(sizeof(GISTENTRY));
			r = new_intArrayType(1);
			gistentryinit(*retval, PointerGetDatum(r), 
				entry->rel, entry->page, entry->offset, VARSIZE(r), FALSE);
			PG_RETURN_POINTER( retval );
		}	
		PG_RETURN_POINTER( entry );
	}

	retval = palloc(sizeof(GISTENTRY));
	r = new_intArrayType( SIGLENINT );

	if (ARRISVOID(in))
	{
		gistentryinit(*retval, PointerGetDatum(r),
			  entry->rel, entry->page, entry->offset, VARSIZE(r), FALSE);
		if (in != (ArrayType *) DatumGetPointer(entry->key))
				pfree(in);
		PG_RETURN_POINTER (retval);
	}

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

	if ( in != (ArrayType *) DatumGetPointer(entry->key))
			pfree(in);

	PG_RETURN_POINTER (retval);
}

Datum
g_intbig_decompress(PG_FUNCTION_ARGS)
{
	GISTENTRY *entry = (GISTENTRY *)PG_GETARG_POINTER(0);
	ArrayType  *key;

	key = (ArrayType *) PG_DETOAST_DATUM(entry->key);

	if ( key != (ArrayType *) DatumGetPointer(entry->key))
	{
		GISTENTRY  *retval;

		retval = palloc(sizeof(GISTENTRY));

		gistentryinit(*retval, PointerGetDatum(key), 
			entry->rel, entry->page, entry->offset, (key) ? VARSIZE(key) : 0, FALSE);
		PG_RETURN_POINTER( retval );
	}
	if ( ARRNELEMS(key) == 1 ) {
		GISTENTRY  *retval;
		ArrayType  *newkey;

		retval = palloc(sizeof(GISTENTRY));
		newkey = new_intArrayType(SIGLENINT);
		MemSet( (void*)ARRPTR(newkey), 0xff, SIGLEN ); 

		gistentryinit(*retval, PointerGetDatum(newkey), 
			entry->rel, entry->page, entry->offset, VARSIZE(newkey), FALSE);
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
		0.1
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

	if ( strategy == BooleanSearchStrategy )
		PG_RETURN_BOOL(signconsistent( (QUERYTYPE*)query, 
						SIGPTR((ArrayType *) DatumGetPointer(entry->key)),
						false ) );
	
	/* XXX what about toasted input? */
	if (ARRISVOID(query))
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

	out->flags &= ~LEAFKEY;
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

typedef struct {
	OffsetNumber	pos;
	float		cost;
} SPLITCOST;

static int 
comparecost( const void *a, const void *b ) {
	if ( ((SPLITCOST*)a)->cost == ((SPLITCOST*)b)->cost )
		return 0;
	else
		return ( ((SPLITCOST*)a)->cost > ((SPLITCOST*)b)->cost ) ? 1 : -1;
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
	SPLITCOST	*costvector;

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
	if ( seed_1 == 0 || seed_2 == 0 ) {
		seed_1 = 1;
		seed_2 = 2;
	}

	datum_alpha = (ArrayType *) DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[seed_1].key);
	datum_l = copy_intArrayType(datum_alpha);
	(*sizef) (datum_l, &size_l);
	datum_beta = (ArrayType *) DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[seed_2].key);
	datum_r = copy_intArrayType(datum_beta);
	(*sizef) (datum_r, &size_r);

	maxoff = OffsetNumberNext(maxoff);
	/*
     	 * sort entries
	 */
	costvector=(SPLITCOST*)palloc( sizeof(SPLITCOST)*maxoff );
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {
		costvector[i-1].pos = i;
		datum_alpha = (ArrayType *) DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[i].key);
		union_d = (*unionf)(datum_l, datum_alpha);
		(*sizef)(union_d, &size_alpha);
		pfree( union_d );
		union_d = (*unionf)(datum_r, datum_alpha);
		(*sizef)(union_d, &size_beta);
		pfree( union_d );
		costvector[i-1].cost = abs( (size_alpha - size_l) - (size_beta - size_r) );
	}
	qsort( (void*)costvector, maxoff, sizeof(SPLITCOST), comparecost );
 
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

	
	for (j = 0; j < maxoff; j++) {
		i = costvector[j].pos;

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
	pfree( costvector );
	*right = *left = FirstOffsetNumber;

	datum_l->flags &= ~LEAFKEY;
	datum_r->flags &= ~LEAFKEY;
	v->spl_ldatum = PointerGetDatum(datum_l);
	v->spl_rdatum = PointerGetDatum(datum_r);

#ifdef GIST_DEBUG
	elog(NOTICE, "--------ENDpicksplit %d %d", v->spl_nleft, v->spl_nright);
#endif
	return v;
}

/*****************************************************************************
 *	 	BoolSearch	
 *****************************************************************************/


#define END     0
#define ERR     1
#define VAL     2
#define OPR     3
#define OPEN    4
#define CLOSE   5

/* parser's states */
#define WAITOPERAND	1
#define WAITENDOPERAND	2
#define WAITOPERATOR	3

/*
 * node of query tree, also used
 * for storing polish notation in parser
 */
typedef struct NODE {
	int4	type;
	int4	val;
	struct NODE 	*next;
} NODE;

typedef struct {
	char *buf;
	int4	state;
	int4  count;
	/* reverse polish notation in list (for temprorary usage)*/
	NODE	*str;
	/* number in str */
	int4	num;
} WORKSTATE;

/*
 * get token from query string
 */
static int4 
gettoken( WORKSTATE* state, int4* val ) {
	char nnn[16], *curnnn;

	curnnn=nnn;
	while(1) {
		switch(state->state) {
			case WAITOPERAND:
				curnnn=nnn;
				if ( (*(state->buf)>='0' && *(state->buf)<='9') || 
						*(state->buf)=='-' ) {
					state->state = WAITENDOPERAND;
					*curnnn = *(state->buf);
					curnnn++;
				} else if ( *(state->buf) == '!' ) {
					(state->buf)++;
					*val = (int4)'!';
					return OPR;
				} else if ( *(state->buf) == '(' ) {
					state->count++;
					(state->buf)++;
					return OPEN;
				} else if ( *(state->buf) != ' ' ) 
					return ERR;
				break;
			case WAITENDOPERAND:
				if ( *(state->buf)>='0' && *(state->buf)<='9' ) {
					*curnnn = *(state->buf);
					curnnn++;
				} else {
					*curnnn = '\0';
					*val=(int4)atoi( nnn );
					state->state = WAITOPERATOR;
					return ( state->count && *(state->buf) == '\0' ) 
						? ERR : VAL;
				}
				break;
			case WAITOPERATOR:
				if ( *(state->buf) == '&' || *(state->buf) == '|' ) {
					state->state = WAITOPERAND;
					*val = (int4) *(state->buf);
	 				(state->buf)++;
					return OPR;
				} else if ( *(state->buf) == ')' ) {
					(state->buf)++;
					state->count--;
					return ( state->count <0 ) ? ERR : CLOSE;
				} else if ( *(state->buf) == '\0' ) {
					return ( state->count ) ? ERR : END;
				} else if ( *(state->buf) != ' ' )
					return ERR;
				break;
	   		default:
				return ERR;
				break;
	 	}
	 	(state->buf)++;
	}
	return END;
}

/*
 * push new one in polish notation reverse view
 */
static void
pushquery( WORKSTATE *state, int4 type, int4 val ) {
	NODE    *tmp = (NODE*)palloc(sizeof(NODE));
	tmp->type=type;
	tmp->val =val;
	tmp->next = state->str;
	state->str = tmp;
	state->num++;
}

#define STACKDEPTH	16

/*
 * make polish notaion of query
 */
static int4 
makepol(WORKSTATE *state) {
	int4 val,type;
	int4	stack[STACKDEPTH];
	int4	lenstack=0;

	while( (type=gettoken(state, &val))!=END ) {
		switch(type) {
			case VAL:
				pushquery(state, type, val);
				while ( lenstack && (stack[ lenstack-1 ] == (int4)'&' || 
						stack[ lenstack-1 ] == (int4)'!') ) {
					lenstack--;
					pushquery(state, OPR, stack[ lenstack ]);
				}
				break;
			case OPR:
				if ( lenstack && val == (int4) '|' ) {
					pushquery(state, OPR, val);
				} else { 
					if ( lenstack == STACKDEPTH )
						elog(ERROR,"Stack too short");
					stack[ lenstack ] = val;
					lenstack++;
				}
				break;
			case OPEN:
				if ( makepol( state ) == ERR ) return ERR;
				if ( lenstack && (stack[ lenstack-1 ] == (int4)'&' || 
						stack[ lenstack-1 ] == (int4)'!') ) {
					lenstack--;
					pushquery(state, OPR, stack[ lenstack ]);
				}
				break;
			case CLOSE:
				while ( lenstack ) {
					lenstack--;
					pushquery(state, OPR, stack[ lenstack ]);
				};
				return END;
				break;
			case ERR:
			default:
				elog(ERROR,"Syntax error");
				return ERR;
			
		}
	}

	while (lenstack) {
		lenstack--;
		pushquery(state, OPR, stack[ lenstack ]);
	};
	return END;
}

typedef struct {
	int4 *arrb;
	int4 *arre;
} CHKVAL;

/*
 * is there value 'val' in array or not ?
 */
static bool
checkcondition_arr( void *checkval, int4 val ) {
	int4      *StopLow = ((CHKVAL*)checkval)->arrb;
	int4      *StopHigh = ((CHKVAL*)checkval)->arre;
	int4      *StopMiddle;

	/* Loop invariant: StopLow <= val < StopHigh */

	while (StopLow < StopHigh) {
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		if (*StopMiddle == val)
			return (true);
		else if (*StopMiddle < val )
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}
	return false;
}

static bool
checkcondition_bit( void *checkval, int4 val ) {
	return GETBIT( checkval, HASHVAL( val ) ); 	
}

/*
 * check for boolean condition
 */
static bool
execute( ITEM* curitem, void *checkval, bool calcnot, bool (*chkcond)(void *checkval, int4 val )) {

	if (  curitem->type == VAL ) {
		return (*chkcond)( checkval, curitem->val );
	} else if ( curitem->val == (int4)'!' ) {
		return ( calcnot ) ? 
			( ( execute(curitem - 1, checkval, calcnot, chkcond) ) ? false : true ) 
			: true;
	} else if ( curitem->val == (int4)'&' ) {
		if ( execute(curitem + curitem->left, checkval, calcnot, chkcond) )
			return execute(curitem - 1, checkval, calcnot, chkcond);
		else
			return false;
	} else { /* |-operator */
		if ( execute(curitem + curitem->left, checkval, calcnot, chkcond) )
			return true;
		else
			return execute(curitem - 1, checkval, calcnot, chkcond);
	}
	return false;
} 

/*
 * signconsistent & execconsistent called by *_consistent
 */
static bool 
signconsistent( QUERYTYPE *query, BITVEC sign, bool calcnot ) {
	return execute( 
		GETQUERY(query) + query->size-1 , 
		(void*)sign, calcnot, 
		checkcondition_bit 
	); 
}

static bool 
execconsistent( QUERYTYPE *query, ArrayType *array, bool calcnot ) {
	CHKVAL  chkval;

	chkval.arrb = ARRPTR(array);
	chkval.arre = chkval.arrb + ARRNELEMS(array);
	return execute( 
		GETQUERY(query) + query->size-1 , 
		(void*)&chkval, calcnot, 
		checkcondition_arr 
	);
}

/*
 * boolean operations 
 */
Datum
rboolop(PG_FUNCTION_ARGS) {
	return DirectFunctionCall2(
		boolop,
		PG_GETARG_DATUM(1),
		PG_GETARG_DATUM(0)
	);
}

Datum
boolop(PG_FUNCTION_ARGS) {
	ArrayType *val = ( ArrayType * )PG_DETOAST_DATUM_COPY(PG_GETARG_POINTER(0));
	QUERYTYPE *query = ( QUERYTYPE * )PG_DETOAST_DATUM(PG_GETARG_POINTER(1));
	CHKVAL  chkval;
	bool result;
	
	if ( ARRISVOID( val ) ) {
		pfree(val);
		PG_FREE_IF_COPY(query,1);
		PG_RETURN_BOOL( false );
	}

	PREPAREARR(val);
	chkval.arrb = ARRPTR(val);
	chkval.arre = chkval.arrb + ARRNELEMS(val);
	result = execute( 
		GETQUERY(query) + query->size-1 , 
		&chkval, true, 
		checkcondition_arr 
	);
	pfree(val);

	PG_FREE_IF_COPY(query,1);
	PG_RETURN_BOOL( result );
}

static void
findoprnd( ITEM *ptr, int4 *pos ) {
#ifdef BS_DEBUG
	elog(NOTICE, ( ptr[*pos].type == OPR ) ? 
		"%d  %c" : "%d  %d ", *pos, ptr[*pos].val );
#endif
	if ( ptr[*pos].type == VAL ) {
		ptr[*pos].left = 0;
		(*pos)--;
	} else if ( ptr[*pos].val == (int4)'!' ) {
		ptr[*pos].left = -1;
		(*pos)--;
		findoprnd( ptr, pos );
	} else {
		ITEM *curitem = &ptr[*pos];
		int4 tmp = *pos; 
		(*pos)--;
		findoprnd(ptr,pos);
		curitem->left = *pos - tmp;
		findoprnd(ptr,pos);
	}
}


/*
 * input
 */
Datum
bqarr_in(PG_FUNCTION_ARGS) {
	char *buf=(char*)PG_GETARG_POINTER(0);
	WORKSTATE state;
	int4 i;
	QUERYTYPE	*query;
	int4 commonlen;
	ITEM *ptr;
	NODE *tmp;
	int4 pos=0;
#ifdef BS_DEBUG
	char pbuf[16384],*cur;
#endif

	state.buf = buf;
	state.state = WAITOPERAND;
	state.count = 0;
	state.num = 0;
	state.str=NULL;

	/* make polish notation (postfix, but in reverse order) */
	makepol( &state );
	if (!state.num) 
		elog( ERROR,"Empty query");

	commonlen = COMPUTESIZE(state.num);
	query = (QUERYTYPE*) palloc( commonlen );
	query->len = commonlen;
	query->size = state.num;
	ptr = GETQUERY(query);

	for(i=state.num-1; i>=0; i-- ) {
		ptr[i].type = state.str->type; 
		ptr[i].val = state.str->val;
		tmp = state.str->next;
		pfree( state.str );
		state.str = tmp;
	}
	
	pos = query->size-1;
	findoprnd( ptr, &pos );
#ifdef BS_DEBUG
	cur = pbuf;
	*cur = '\0';
	for( i=0;i<query->size;i++ ) {
		if ( ptr[i].type == OPR ) 
			sprintf(cur, "%c(%d) ", ptr[i].val, ptr[i].left);
		else 
			sprintf(cur, "%d ", ptr[i].val );
		cur = strchr(cur,'\0');	
	}
	elog(NOTICE,"POR: %s", pbuf);
#endif

	PG_RETURN_POINTER( query );
}


/*
 * out function
 */
typedef struct {
	ITEM    *curpol;
	char *buf;
	char *cur;
	int4 buflen;
} INFIX;

#define RESIZEBUF(inf,addsize) while( ( inf->cur - inf->buf ) + addsize + 1 >= inf->buflen ) { \
	int4 len = inf->cur - inf->buf; \
	inf->buflen *= 2; \
	inf->buf = (char*) repalloc( (void*)inf->buf, inf->buflen ); \
	inf->cur = inf->buf + len; \
}

static void
infix(INFIX *in, bool first) {
	if ( in->curpol->type == VAL ) {
		RESIZEBUF(in, 11);
		sprintf(in->cur, "%d", in->curpol->val );
		in->cur = strchr( in->cur, '\0' );
		in->curpol--;
	} else if ( in->curpol->val == (int4)'!' ) {
		bool isopr = false;
		RESIZEBUF(in, 1);
		*(in->cur) = '!';
		in->cur++;
		*(in->cur) = '\0';
		in->curpol--;
		if ( in->curpol->type == OPR ) {
			isopr = true;
			RESIZEBUF(in, 2);
			sprintf(in->cur, "( ");
			in->cur = strchr( in->cur, '\0' );
		} 
		infix( in, isopr );
		if ( isopr ) {
			RESIZEBUF(in, 2);
			sprintf(in->cur, " )");
			in->cur = strchr( in->cur, '\0' );
		} 
	} else {
		int4 op = in->curpol->val;
		INFIX   nrm;
		
		in->curpol--;
		if ( op == (int4)'|' && ! first) {
			RESIZEBUF(in, 2);
			sprintf(in->cur, "( ");
			in->cur = strchr( in->cur, '\0' );
		}

		nrm.curpol = in->curpol;
		nrm.buflen = 16;
		nrm.cur = nrm.buf = (char*)palloc( sizeof(char) * nrm.buflen );
		
		/* get right operand */
		infix( &nrm, false );
		
		/* get & print left operand */
		in->curpol = nrm.curpol;
		infix( in, false );

		/* print operator & right operand*/
		RESIZEBUF(in, 3 + (nrm.cur - nrm.buf) );
		sprintf(in->cur, " %c %s", op, nrm.buf);
		in->cur = strchr( in->cur, '\0' );
		pfree( nrm.buf );

		if ( op == (int4)'|' && ! first) {
			RESIZEBUF(in, 2);
			sprintf(in->cur, " )");
			in->cur = strchr( in->cur, '\0' );
		}
	}
}


Datum
bqarr_out(PG_FUNCTION_ARGS) {
	QUERYTYPE       *query = (QUERYTYPE*)PG_DETOAST_DATUM(PG_GETARG_POINTER(0));
	INFIX   nrm;

	if ( query->size == 0 )
		elog(ERROR,"Empty");	
	nrm.curpol = GETQUERY(query) + query->size - 1;
	nrm.buflen = 32;
	nrm.cur = nrm.buf = (char*)palloc( sizeof(char) * nrm.buflen );
	*(nrm.cur) = '\0';
	infix( &nrm, true );
	
	PG_FREE_IF_COPY(query,0);
	PG_RETURN_POINTER( nrm.buf );
}

static int4
countdroptree( ITEM *q, int4 pos ) {
	if ( q[pos].type == VAL ) {
		return 1;
	} else if ( q[pos].val == (int4)'!' ) {
		return 1+countdroptree(q, pos-1);
	} else {
		return 1 + countdroptree(q, pos-1) + countdroptree(q, pos + q[pos].left); 
	} 
}

/*
 * common algorithm:
 * result of all '!' will be = 'true', so 
 * we can modify query tree for clearing
 */
static int4 
shorterquery( ITEM *q, int4 len ) {
	int4 index,posnot,poscor;
	bool notisleft = false;
	int4 drop,i;

	/* out all '!' */
	do {
		index=0;
		drop=0;
		/* find ! */
		for(posnot=0; posnot < len; posnot++)
			if ( q[posnot].type == OPR  && q[posnot].val == (int4)'!') {
				index=1;
				break;
			}

		if ( posnot == len )
			return len;

		/* last operator is ! */
		if ( posnot == len-1 )
			return 0;

		/* find operator for this operand */
		for( poscor=posnot+1; poscor<len; poscor++) {
			if ( q[poscor].type == OPR ) {
				if ( poscor == posnot+1 ) {
					notisleft = false;
					break;
				} else if ( q[poscor].left + poscor == posnot ) { 
					notisleft = true;
					break;
				}
			}
		}
		if ( q[poscor].val == (int4)'!' ) {
			drop = countdroptree(q, poscor);
			q[poscor-1].type=VAL;
			for(i=poscor+1;i<len;i++)
				if ( q[i].type == OPR && q[i].left + i <= poscor )
					q[i].left += drop - 2; 
			memcpy( (void*)&q[poscor-drop+1], 
				(void*)&q[poscor-1], 
				sizeof(ITEM) * ( len - (poscor-1) ));
			len -= drop - 2;
		} else if ( q[poscor].val == (int4)'|' ) {
			drop = countdroptree(q, poscor);
			q[poscor-1].type=VAL;
			q[poscor].val=(int4)'!';
			q[poscor].left=-1;
			for(i=poscor+1;i<len;i++)
				if ( q[i].type == OPR && q[i].left + i < poscor )
					q[i].left += drop - 2; 
			memcpy( (void*)&q[poscor-drop+1], 
				(void*)&q[poscor-1], 
				sizeof(ITEM) * ( len - (poscor-1) ));
			len -= drop - 2;
		} else { /* &-operator */
			if ( 
					(notisleft && q[poscor-1].type == OPR && 
						q[poscor-1].val == (int4)'!' ) ||
					(!notisleft && q[poscor+q[poscor].left].type == OPR && 
						q[poscor+q[poscor].left].val == (int4)'!' )
				) { /* drop subtree */
				drop = countdroptree(q, poscor);
				q[poscor-1].type=VAL;
				q[poscor].val=(int4)'!';
				q[poscor].left=-1;
				for(i=poscor+1;i<len;i++)
					if ( q[i].type == OPR && q[i].left + i < poscor )
						q[i].left += drop - 2; 
				memcpy( (void*)&q[poscor-drop+1], 
					(void*)&q[poscor-1], 
					sizeof(ITEM) * ( len - (poscor-1) ));
				len -= drop - 2;
			} else { /* drop only operator */
				int4 subtreepos = ( notisleft ) ? 
					poscor-1 : poscor+q[poscor].left;
				int4 subtreelen = countdroptree( q, subtreepos );
				drop = countdroptree(q, poscor);
				for(i=poscor+1;i<len;i++)
					if ( q[i].type == OPR && q[i].left + i < poscor )
						q[i].left += drop - subtreelen; 
				memcpy( (void*)&q[ subtreepos+1 ], 
					(void*)&q[poscor+1], 
					sizeof(ITEM)*( len - (poscor-1) ) ); 
				memcpy( (void*)&q[ poscor-drop+1 ], 
					(void*)&q[subtreepos-subtreelen+1], 
					sizeof(ITEM)*( len - (drop-subtreelen) ) );
				len -= drop - subtreelen;  
			}
		}
	} while( index );
	return len;
}


Datum
querytree(PG_FUNCTION_ARGS) {
	QUERYTYPE       *query = (QUERYTYPE*)PG_DETOAST_DATUM(PG_GETARG_POINTER(0));
	INFIX   nrm;
	text 	*res;
	ITEM	*q;
	int4	len;

	if ( query->size == 0 )
		elog(ERROR,"Empty");

	q = (ITEM*)palloc( sizeof(ITEM) * query->size );
	memcpy( (void*)q, GETQUERY(query), sizeof(ITEM) * query->size );
	len = shorterquery( q, query->size );
	PG_FREE_IF_COPY(query,0);

	if ( len == 0 ) {
		res = (text*) palloc( 1 + VARHDRSZ );
		VARATT_SIZEP(res) = 1 + VARHDRSZ;
		*((char*)VARDATA(res)) = 'T';
	} else {
		nrm.curpol = q + len - 1;
		nrm.buflen = 32;
		nrm.cur = nrm.buf = (char*)palloc( sizeof(char) * nrm.buflen );
		*(nrm.cur) = '\0';
		infix( &nrm, true );

		res = (text*) palloc( nrm.cur-nrm.buf + VARHDRSZ );
		VARATT_SIZEP(res) = nrm.cur-nrm.buf + VARHDRSZ;
		strncpy( VARDATA(res), nrm.buf, nrm.cur-nrm.buf );
	}
	pfree(q);

	PG_RETURN_POINTER( res );
}
