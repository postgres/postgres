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

#define max(a,b)        ((a) >  (b) ? (a) : (b))
#define min(a,b)        ((a) <= (b) ? (a) : (b))
#define abs(a)          ((a) <  (0) ? -(a) : (a))

/* dimension of array */
#define NDIM 1

/* useful macros for accessing int4 arrays */
#define ARRPTR(x)  ( (int4 *) ARR_DATA_PTR(x) )
#ifdef PGSQL71
#define ARRNELEMS(x)  ArrayGetNItems( ARR_NDIM(x), ARR_DIMS(x))
#else
#define ARRNELEMS(x)  getNitems( ARR_NDIM(x), ARR_DIMS(x))
#endif

#define ARRISNULL(x) ( (x) ? ( ( ARR_NDIM(x) == NDIM ) ? ( ( ARRNELEMS( x ) ) ? 0 : 1 ) : 1  ) : 1 )

#define SORT(x) \
	do { \
		 if ( ARRNELEMS( x ) > 1 ) \
			isort( (void*)ARRPTR( x ), ARRNELEMS( x ) ); \
	} while(0)

#define PREPAREARR(x) \
	do { \
		 if ( ARRNELEMS( x ) > 1 ) \
			if ( isort( (void*)ARRPTR( x ), ARRNELEMS( x ) ) ) \
				x = _int_unique( x ); \
	} while(0)

/* "wish" function */
#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )


/* bigint defines */
#define BITBYTE 8
#define SIGLENINT  64   /* >122 => key will toast, so very slow!!! */
#define SIGLEN  ( sizeof(int)*SIGLENINT )
#define SIGLENBIT (SIGLEN*BITBYTE)

typedef char BITVEC[SIGLEN];
typedef char* BITVECP;
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
static void printarr ( ArrayType * a, int num ) {
	char bbb[16384];
	char *cur;
	int l;
	int *d;
	d = ARRPTR( a );
	*bbb = '\0';
	cur = bbb;
	for(l=0; l<min( num, ARRNELEMS( a ));l++) {
		sprintf(cur,"%d ", d[l] );
		cur = strchr( cur, '\0' ) ;
	}
	elog(NOTICE, "\t\t%s", bbb);
}
static void printbitvec( BITVEC bv ) {
        int i;
	char str[ SIGLENBIT+1 ];
	str[ SIGLENBIT ] ='\0';
        LOOPBIT( str[i] = ( GETBIT(bv,i) ) ? '1' : '0' );
	
	elog(NOTICE,"BV: %s", str);
}
#endif

/*
** types for functions
*/
typedef ArrayType * (*formarray) (ArrayType*, ArrayType*);
typedef void (*formfloat) (ArrayType*, float*);

/*
** usefull function
*/
static bool isort( int *a, const int len );
static ArrayType * new_intArrayType( int num );
static ArrayType * copy_intArrayType( ArrayType * a );
static ArrayType * resize_intArrayType( ArrayType * a, int num );
static int internal_size( int *a, int len );
static ArrayType * _int_unique( ArrayType * a );

/* common GiST function*/
static GIST_SPLITVEC *  _int_common_picksplit(bytea *entryvec, 
		GIST_SPLITVEC *v, 
		formarray unionf, 
		formarray interf, 
		formfloat sizef,
		float coef);
static float * _int_common_penalty(GISTENTRY *origentry, 
		GISTENTRY *newentry, 
		float *result,
		formarray unionf,
		formfloat sizef);
static ArrayType * _int_common_union(bytea *entryvec, 
		int *sizep, 
		formarray unionf);

/* 
** GiST support methods
*/
bool             g_int_consistent(GISTENTRY *entry, ArrayType *query, StrategyNumber strategy);
GISTENTRY *      g_int_compress(GISTENTRY *entry);
GISTENTRY *      g_int_decompress(GISTENTRY *entry);
float *          g_int_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result);
GIST_SPLITVEC *  g_int_picksplit(bytea *entryvec, GIST_SPLITVEC *v);
ArrayType *            g_int_union(bytea *entryvec, int *sizep);
bool *           g_int_same(ArrayType *b1, ArrayType *b2, bool *result);


/*
** R-tree support functions
*/
bool     inner_int_contains(ArrayType *a, ArrayType *b);
bool     inner_int_overlap(ArrayType *a, ArrayType *b);
ArrayType *    inner_int_union(ArrayType *a, ArrayType *b);
ArrayType *    inner_int_inter(ArrayType *a, ArrayType *b);

bool     _int_different(ArrayType *a, ArrayType *b);
bool     _int_same(ArrayType *a, ArrayType *b);
bool     _int_contains(ArrayType *a, ArrayType *b);
bool     _int_contained(ArrayType *a, ArrayType *b);
bool     _int_overlap(ArrayType *a, ArrayType *b);
ArrayType *    _int_union(ArrayType *a, ArrayType *b);
ArrayType *    _int_inter(ArrayType *a, ArrayType *b);
void     rt__int_size(ArrayType *a, float* sz);

/*
** _intbig methods
*/
bool             g_intbig_consistent(GISTENTRY *entry, ArrayType *query, StrategyNumber strategy);
GISTENTRY *      g_intbig_compress(GISTENTRY *entry);
GISTENTRY *      g_intbig_decompress(GISTENTRY *entry);
float *          g_intbig_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result);
GIST_SPLITVEC *  g_intbig_picksplit(bytea *entryvec, GIST_SPLITVEC *v);
ArrayType *      g_intbig_union(bytea *entryvec, int *sizep);
bool *           g_intbig_same(ArrayType *a, ArrayType *b, bool *result);

static bool     _intbig_contains(ArrayType *a, ArrayType *b);
static bool     _intbig_overlap(ArrayType *a, ArrayType *b);
static ArrayType *    _intbig_union(ArrayType *a, ArrayType *b);
/*static ArrayType *    _intbig_inter(ArrayType *a, ArrayType *b);*/
static void     rt__intbig_size(ArrayType *a, float* sz);
static void gensign(BITVEC sign, int * a, int len);

/*****************************************************************************
 *                         GiST functions
 *****************************************************************************/

/*
** The GiST Consistent method for _intments
** Should return false if for all data items x below entry,
** the predicate x op query == FALSE, where op is the oper
** corresponding to strategy in the pg_amop table.
*/
bool 
g_int_consistent(GISTENTRY *entry,
	       ArrayType *query,
	       StrategyNumber strategy)
{
    bool retval;
   
    /* sort query for fast search, key is already sorted */
	/* XXX are we sure it's safe to scribble on the query object here? */
	/* XXX what about toasted input? */
    if ( ARRISNULL( query ) ) return FALSE; 
    PREPAREARR( query );    

    switch(strategy) {
    case RTOverlapStrategyNumber:
      retval = (bool)inner_int_overlap((ArrayType *)(entry->pred), query);
      break;
    case RTSameStrategyNumber:
    case RTContainsStrategyNumber:
      retval = (bool)inner_int_contains((ArrayType *)(entry->pred), query);
      break;
    case RTContainedByStrategyNumber:
      retval = (bool)inner_int_overlap((ArrayType *)(entry->pred), query);
      break;
    default:
      retval = FALSE;
    }
    return(retval);
}

ArrayType *
g_int_union(bytea *entryvec, int *sizep)
{
	return _int_common_union( entryvec, sizep, inner_int_union );
}

/*
** GiST Compress and Decompress methods
*/
GISTENTRY *
g_int_compress(GISTENTRY *entry)
{
    GISTENTRY *retval;
    ArrayType * r;
    int len;
    int *dr;
    int i,min,cand;

    retval = palloc(sizeof(GISTENTRY));

#ifdef PGSQL71
	if ( entry->pred ) 
		r = (ArrayType *)PG_DETOAST_DATUM_COPY( entry->pred );
	else 
		r = NULL;
#else
    r = copy_intArrayType( (ArrayType *) entry->pred );
#endif

    if ( ARRISNULL( r ) )  {
#ifdef GIST_DEBUG
	elog(NOTICE,"COMP IN: NULL"); 
#endif
	if ( r ) if ( (char*)r != (char*)entry->pred ) pfree(r);

	gistentryinit(*retval, (char *)NULL, entry->rel, entry->page, entry->offset, 
				  0, FALSE);
	return( retval ); 
    }
 
    if ( entry->leafkey ) PREPAREARR( r );
    len = ARRNELEMS( r );

#ifdef GIST_DEBUG
    elog(NOTICE, "COMP IN: %d leaf; %d rel; %d page; %d offset; %d bytes; %d elems", entry->leafkey, (int)entry->rel, (int)entry->page, (int)entry->offset, (int)entry->bytes, len);
#endif

    if ( len >= 2*MAXNUMRANGE ) {  /*compress*/
    	r = resize_intArrayType( r, 2*( len ) );
   
    	dr = ARRPTR( r );

    	for(i=len-1; i>=0;i--)
    		dr[2*i] = dr[2*i+1] = dr[i];
    
    	len *= 2;
    	cand = 1;
    	while( len > MAXNUMRANGE * 2 ) {
		min = 0x7fffffff;
		for( i=2; i<len;i+=2 )
			if ( min > (dr[i] - dr[i-1]) ) {
				min = (dr[i] - dr[i-1]);
				cand = i;
			}
		memmove( (void*)&dr[cand-1], (void*)&dr[cand+1], (len - cand - 1)*sizeof(int) );
		len -= 2;
	}
    	r = resize_intArrayType(r, len );
    }

    gistentryinit(*retval, (char *)r, entry->rel, entry->page, entry->offset, VARSIZE( r ), FALSE);

    return(retval);
}

GISTENTRY *
g_int_decompress(GISTENTRY *entry)
{
    GISTENTRY *retval;
    ArrayType * r; 
    int *dr, lenr;
    ArrayType * in; 
    int lenin;
    int *din;
    int i,j;

#ifdef PGSQL71
	if ( entry->pred ) 
		in = (ArrayType *)PG_DETOAST_DATUM( entry->pred );
	else 
		in = NULL;
#else
	in = (ArrayType *) entry->pred;
#endif

    if ( entry->bytes < ARR_OVERHEAD( NDIM ) || ARRISNULL( in ) ) { 
    	retval = palloc(sizeof(GISTENTRY));

#ifdef PGSQL71	
	if ( in ) if ( (char*)in != (char*)entry->pred ) pfree(in);
#endif
    	gistentryinit(*retval, (char *)NULL, entry->rel, entry->page, entry->offset, 0, FALSE);
#ifdef GIST_DEBUG
	elog(NOTICE,"DECOMP IN: NULL"); 
#endif
	return( retval ); 
    }
    

    lenin = ARRNELEMS(in);
    din = ARRPTR(in);

    if ( lenin < 2*MAXNUMRANGE ) { /*not comressed value*/
	/* sometimes strange bytesize */
    	gistentryinit(*entry, (char *)in, entry->rel, entry->page, entry->offset, VARSIZE( in ), FALSE);
	return (entry);
    }

#ifdef GIST_DEBUG
    elog(NOTICE, "DECOMP IN: %d leaf; %d rel; %d page; %d offset; %d bytes; %d elems", entry->leafkey, (int)entry->rel, (int)entry->page, (int)entry->offset, (int)entry->bytes, lenin);
#endif

    lenr = internal_size(din, lenin);

    r = new_intArrayType( lenr );
    dr = ARRPTR( r );

    for(i=0;i<lenin;i+=2)
	for(j=din[i]; j<=din[i+1]; j++)
		if ( (!i) || *(dr-1) != j )
			*dr++ = j;

#ifdef PGSQL71	
    if ( (char*)in != (char*)entry->pred ) pfree(in);
#endif
    retval = palloc(sizeof(GISTENTRY));

    gistentryinit(*retval, (char *)r, entry->rel, entry->page, entry->offset, VARSIZE( r ), FALSE);

    return(retval);
}

/*
** The GiST Penalty method for _intments
*/
float *
g_int_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result)
{
    return _int_common_penalty( origentry, newentry, result, inner_int_union, rt__int_size);
}


GIST_SPLITVEC *
g_int_picksplit(bytea *entryvec,
	      GIST_SPLITVEC *v)
{
	return _int_common_picksplit( entryvec, v, 
		inner_int_union, 
		inner_int_inter,
		rt__int_size,
		1e-8);  
}

/*
** Equality methods
*/


bool *
g_int_same(ArrayType *b1, ArrayType *b2, bool *result)
{
  if (_int_same(b1, b2))
    *result = TRUE;
  else *result = FALSE;

  return(result);
}

bool
_int_contained(ArrayType *a, ArrayType *b)
{
  return ( _int_contains(b, a) );
}

bool
_int_contains ( ArrayType *a, ArrayType *b ) {
	bool res;
	ArrayType *an, *bn;

	if ( ARRISNULL( a ) || ARRISNULL( b ) ) return FALSE;

	an = copy_intArrayType( a );
	bn = copy_intArrayType( b );

	PREPAREARR(an);
	PREPAREARR(bn);

        res = inner_int_contains( an, bn );
	pfree( an ); pfree( bn );
	return res;
}

bool 
inner_int_contains ( ArrayType *a, ArrayType *b ) {
	int na, nb;
	int i,j, n;
	int *da, *db;
  
        if ( ARRISNULL( a ) || ARRISNULL( b ) ) return FALSE;

        na = ARRNELEMS( a );
	nb = ARRNELEMS( b );	
	da = ARRPTR( a );
	db = ARRPTR( b );

#ifdef GIST_DEBUG
    elog(NOTICE, "contains %d %d", na, nb);
#endif

	i = j = n = 0;
	while( i<na && j<nb )
		if ( da[i] < db[j] )
			i++;
		else if ( da[i] == db[j] ) {
			n++; i++; j++;
		} else 
			j++;
	
	return ( n == nb ) ? TRUE : FALSE;
}

/*****************************************************************************
 * Operator class for R-tree indexing
 *****************************************************************************/

bool
_int_different(ArrayType *a, ArrayType *b)
{
  return ( !_int_same( a, b ) );
}

bool 
_int_same ( ArrayType *a, ArrayType *b ) {
        int na , nb ;
        int n; 
        int *da, *db;
		bool result;
	ArrayType *an, *bn;
	bool anull = ARRISNULL( a );
	bool bnull = ARRISNULL( b );

	if ( anull || bnull ) 
		return ( anull && bnull ) ? TRUE : FALSE; 

	an = copy_intArrayType( a );
	bn = copy_intArrayType( b );
	
	SORT( an );
	SORT( bn );		
	na = ARRNELEMS( an );
	nb = ARRNELEMS( bn );
	da = ARRPTR( an );
	db = ARRPTR( bn );

	result = FALSE;

	if ( na == nb )
	{
		result = TRUE;
        for(n=0; n<na; n++)
                if ( da[n] != db[n] )
				{
					result = FALSE;
					break;
				}
	}

	pfree( an ); pfree( bn );

	return result;
}

/*  _int_overlap -- does a overlap b?
 */
bool 
_int_overlap ( ArrayType *a, ArrayType *b ) {
	bool result;
	ArrayType *an, *bn;

	if ( ARRISNULL( a ) || ARRISNULL( b ) ) return FALSE;
	
	an = copy_intArrayType( a );
	bn = copy_intArrayType( b );

	SORT(an);
	SORT(bn);

	result = inner_int_overlap( an, bn );

	pfree( an ); pfree( bn );

	return result;
}

bool 
inner_int_overlap ( ArrayType *a, ArrayType *b ) {
	int na , nb ;
	int i,j;
	int *da, *db;

	if ( ARRISNULL( a ) || ARRISNULL( b ) ) return FALSE;
	
	na = ARRNELEMS( a );
	nb = ARRNELEMS( b );
	da = ARRPTR( a );
	db = ARRPTR( b );

#ifdef GIST_DEBUG
    elog(NOTICE, "g_int_overlap");
#endif

	i = j = 0;
	while( i<na && j<nb )
		if ( da[i] < db[j] )
			i++;
		else if ( da[i] == db[j] )
			return TRUE; 
		else 
			j++;
	
	return FALSE;
}

ArrayType * 
_int_union ( ArrayType *a, ArrayType *b ) {
	ArrayType *result;
	ArrayType *an, *bn;
	
	an = copy_intArrayType( a );
	bn = copy_intArrayType( b );

	if ( ! ARRISNULL( an ) ) SORT(an);
	if ( ! ARRISNULL( bn ) ) SORT(bn);

	result = inner_int_union( an, bn );

	if (an) pfree( an );
	if (bn) pfree( bn );

	return result;
}

ArrayType * 
inner_int_union ( ArrayType *a, ArrayType *b ) {
	ArrayType * r = NULL;
	int na , nb;
	int *da, *db, *dr;
	int i,j;

#ifdef GIST_DEBUG
    elog(NOTICE, "inner_union %d %d", ARRISNULL( a ) , ARRISNULL( b ) );
#endif

	if ( ARRISNULL( a ) && ARRISNULL( b ) ) return new_intArrayType(0);
	if ( ARRISNULL( a ) ) r = copy_intArrayType( b ); 
	if ( ARRISNULL( b ) ) r = copy_intArrayType( a ); 

	if ( r ) { 
		dr = ARRPTR( r );
	} else {
		na = ARRNELEMS( a );
		nb = ARRNELEMS( b );
		da = ARRPTR( a );
		db = ARRPTR( b );

		r = new_intArrayType( na + nb ); 
		dr = ARRPTR( r );

		/* union */	
		i = j = 0;
		while( i<na && j<nb ) 
			if ( da[i] < db[j] )
				*dr++ = da[i++];
			else
				*dr++ = db[j++];
	
		while( i<na ) *dr++ = da[i++];
		while( j<nb ) *dr++ = db[j++];

	}	

	if ( ARRNELEMS(r) > 1 ) 
		r = _int_unique( r );

	return r;
}


ArrayType * 
_int_inter ( ArrayType *a, ArrayType *b ) {
	ArrayType *result;
	ArrayType *an, *bn;
	
	if ( ARRISNULL( a ) || ARRISNULL( b ) ) return new_intArrayType(0);

	an = copy_intArrayType( a );
	bn = copy_intArrayType( b );

	SORT(an);
	SORT(bn);

	result = inner_int_inter( an, bn );

	pfree( an ); pfree( bn );

	return result;
}

ArrayType * 
inner_int_inter ( ArrayType *a, ArrayType *b ) {
	ArrayType * r;
	int na , nb ;
	int *da, *db, *dr;
	int i,j;

#ifdef GIST_DEBUG
    elog(NOTICE, "inner_inter %d %d", ARRISNULL( a ), ARRISNULL( b ) );
#endif

	if ( ARRISNULL( a ) || ARRISNULL( b ) ) return new_intArrayType(0);

	na = ARRNELEMS( a );
	nb = ARRNELEMS( b );
	da = ARRPTR( a );
	db = ARRPTR( b );
	r = new_intArrayType( min(na, nb) ); 
	dr = ARRPTR( r );
	
	i = j = 0;
	while( i<na && j<nb ) 
		if ( da[i] < db[j] )
			i++;
		else if ( da[i] == db[j] ) { 
			if ( i+j == 0 || ( i+j>0 && *(dr-1) != db[j] ) )  
				*dr++ = db[j];
			i++; j++;
		} else 
			j++;

	if ( (dr - ARRPTR(r)) == 0 ) {
		pfree( r );
		return new_intArrayType(0);
	} else 
		return resize_intArrayType(r, dr - ARRPTR(r) );
}

void
rt__int_size(ArrayType *a, float *size)
{
  if ( ARRISNULL( a ) )
    *size = 0.0;
  else
    *size = (float)ARRNELEMS( a );
  
  return;
}


/*****************************************************************************
 *                 Miscellaneous operators and functions
 *****************************************************************************/

/* len >= 2 */
static bool 
isort ( int *a, int len ) {
        int tmp, index;
        int *cur, *end;
	bool r = FALSE;
        end = a + len;
        do {
                index = 0;
                cur = a + 1;
                while( cur < end ) {
                        if( *(cur-1) > *cur ) {
                                tmp=*(cur-1); *(cur-1) = *cur; *cur=tmp;
                                index = 1;
                        } else if ( ! r && *(cur-1) == *cur )
				r = TRUE;
                        cur++;
                }
        } while( index );
	return r;
}

static ArrayType * 
new_intArrayType( int num ) {
	ArrayType * r;
	int nbytes = ARR_OVERHEAD( NDIM ) + sizeof(int)*num;
	
	r = (ArrayType *) palloc( nbytes );

	MemSet(r, 0, nbytes);
	r->size = nbytes;
	r->ndim = NDIM;
#ifndef PGSQL71
	SET_LO_FLAG(false, r);
#endif
	*( (int*)ARR_DIMS(r) ) = num;
	*( (int*)ARR_LBOUND(r) ) = 1;
	
	return r;	
} 

static ArrayType * 
resize_intArrayType( ArrayType * a, int num ) {
	int nbytes = ARR_OVERHEAD( NDIM ) + sizeof(int)*num;

	if ( num == ARRNELEMS(a) ) return a;

	a = (ArrayType *) repalloc( a, nbytes );
	
	a->size = nbytes;
	*( (int*)ARR_DIMS(a) ) = num; 
	return a;
}

static ArrayType * 
copy_intArrayType( ArrayType * a ) {
	ArrayType * r;
	if ( ARRISNULL(a) ) return NULL;
	r = new_intArrayType( ARRNELEMS(a) );
	memmove(r,a,VARSIZE(a));
	return r;
}

/* num for compressed key */
static int 
internal_size (int *a, int len ) {
        int i,size=0;

        for(i=0;i<len;i+=2)
                if ( ! i || a[i] != a[i-1]  ) /* do not count repeated range */
                        size += a[i+1] - a[i] + 1;

        return size;
}

/* r is sorted and size of r > 1 */
static ArrayType * 
_int_unique( ArrayType * r ) {
	int *tmp, *dr, *data;
	int num = ARRNELEMS(r);
	data = tmp = dr = ARRPTR( r );
	while( tmp - data < num ) 
		if ( *tmp != *dr ) 
			*(++dr) = *tmp++;
		else 
			tmp++; 
	return resize_intArrayType(r, dr + 1 - ARRPTR(r) );
}

/*********************************************************************
** intbig functions
*********************************************************************/
static void 
gensign(BITVEC sign, int * a, int len) {
        int i;
		/* we assume that the sign vector is previously zeroed */
		for(i=0; i<len; i++) {
			SETBIT( sign, (*a)%SIGLENBIT );
			a++;
		}
}

static bool  
_intbig_overlap(ArrayType *a, ArrayType *b) {
	int i;
	BITVECP da, db;
	
	if ( ARRISNULL( a ) || ARRISNULL( b ) ) return FALSE;
	da = SIGPTR(a);
	db = SIGPTR(b);

	LOOPBYTE( if ( da[i] & db[i] ) return TRUE );
	return FALSE;
}

static bool     
_intbig_contains(ArrayType *a, ArrayType *b) {
	int i;
	BITVECP da, db;
	
	if ( ARRISNULL( a ) || ARRISNULL( b ) ) return FALSE;
	da = SIGPTR(a);
	db = SIGPTR(b);

	LOOPBYTE( if ( db[i] & ~da[i] ) return FALSE );

	return TRUE;
}

static void     
rt__intbig_size(ArrayType *a, float* sz) {
	int i, len=0;
	BITVECP bv;

	if ( ARRISNULL( a ) ) {
		*sz=0.0;
		return;
	}

	bv = SIGPTR(a);
	LOOPBIT( len += GETBIT(bv, i) );
	*sz = (float) len;
	return;
}

static ArrayType *    
_intbig_union(ArrayType *a, ArrayType *b) {
        ArrayType * r;
        BITVECP da, db, dr;
        int i;
       
        if ( ARRISNULL( a ) && ARRISNULL( b ) ) return new_intArrayType(0);
        if ( ARRISNULL( a ) ) return copy_intArrayType( b );
        if ( ARRISNULL( b ) ) return copy_intArrayType( a );
	
	r = new_intArrayType( SIGLENINT );

	da = SIGPTR(a);
	db = SIGPTR(b);
	dr = SIGPTR(r);

	LOOPBYTE(dr[i] = da[i] | db[i]);

        return r;
} 

static ArrayType *    
_intbig_inter(ArrayType *a, ArrayType *b) {
        ArrayType * r;
        BITVECP da, db, dr;
        int i;

        if ( ARRISNULL( a ) || ARRISNULL( b ) ) return new_intArrayType(0);

	r = new_intArrayType( SIGLENINT );

	da = SIGPTR(a);
	db = SIGPTR(b);
	dr = SIGPTR(r);

	LOOPBYTE(dr[i] = da[i] & db[i]);

        return r;
} 

bool *
g_intbig_same(ArrayType *a, ArrayType *b, bool *result) {
	BITVECP da, db;
	int i;

	if ( ARRISNULL( a ) || ARRISNULL( b ) ) {
		*result = ( ARRISNULL( a ) && ARRISNULL( b ) ) ? TRUE : FALSE;
		return result;
	}

	da = SIGPTR(a);
	db = SIGPTR(b);

	LOOPBYTE(
		if ( da[i] != db[i] ) {
			*result = FALSE;
			return result;
		}
	);

	*result = TRUE;
	return result;
}

GISTENTRY *      
g_intbig_compress(GISTENTRY *entry) {
	GISTENTRY *retval;
	ArrayType *r, *in;
#ifdef PGSQL71
	if ( entry->pred ) 
		in = (ArrayType *)PG_DETOAST_DATUM( entry->pred );
	else 
		in = NULL;
#else
	in = (ArrayType *) entry->pred;
#endif

	if ( ! entry->leafkey ) return entry;
	
	retval = palloc(sizeof(GISTENTRY));

	if ( ARRISNULL( in ) )  {
#ifdef PGSQL71	
		if ( in ) if ( (char*)in != (char*)entry->pred ) pfree(in);
#endif
		gistentryinit(*retval, (char *)NULL, entry->rel, entry->page, entry->offset,0, FALSE);
		return( retval );
	}
	
	r = new_intArrayType( SIGLENINT );
	gensign( SIGPTR( r ), 
		 ARRPTR ( in ),
		 ARRNELEMS( in ) );
	
	gistentryinit(*retval, (char *)r, entry->rel, entry->page, entry->offset, VARSIZE( r ), FALSE);

#ifdef PGSQL71	
	if ( in ) if ( (char*)in != (char*)entry->pred ) pfree(in);
#endif

	return(retval);
}

GISTENTRY *      
g_intbig_decompress(GISTENTRY *entry) {
#ifdef PGSQL71
	ArrayType *key;
	key = (ArrayType *)PG_DETOAST_DATUM( entry->pred );
	if ( (char*)key != (char*)entry->pred ) {
		GISTENTRY *retval;
		retval = palloc(sizeof(GISTENTRY));

		gistentryinit(*retval, (char *)key, entry->rel, entry->page, entry->offset, VARSIZE( key ), FALSE);
		return retval;
	}
#endif
	return entry;
}

GIST_SPLITVEC *  
g_intbig_picksplit(bytea *entryvec, GIST_SPLITVEC *v) {
        return _int_common_picksplit( entryvec, v,
                _intbig_union,
                _intbig_inter,
                rt__intbig_size,
		1.0);   

}

ArrayType *      
g_intbig_union(bytea *entryvec, int *sizep) {
    return _int_common_union( entryvec, sizep, _intbig_union );
}

float *          
g_intbig_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result){
    _int_common_penalty( origentry, newentry, result, _intbig_union, rt__intbig_size);
    return result;
}

bool             
g_intbig_consistent(GISTENTRY *entry, ArrayType *query, StrategyNumber strategy) {
    bool retval;
    ArrayType * q;

	/* XXX what about toasted input? */
    if ( ARRISNULL( query ) ) return FALSE;

    q = new_intArrayType( SIGLENINT );
    gensign( 	SIGPTR( q ),
		ARRPTR( query ),
		ARRNELEMS( query ) );

    switch(strategy) {
    case RTOverlapStrategyNumber:
      retval = (bool)_intbig_overlap((ArrayType *)(entry->pred), q);
      break;
    case RTSameStrategyNumber:
    case RTContainsStrategyNumber:
      retval = (bool)_intbig_contains((ArrayType *)(entry->pred), q);
      break;
    case RTContainedByStrategyNumber:
      retval = (bool)_intbig_overlap((ArrayType *)(entry->pred), q);
      break;
    default:
      retval = FALSE;
    }
    pfree( q );
    return(retval);
}

/*****************************************************************
** Common GiST Method
*****************************************************************/

/*
** The GiST Union method for _intments
** returns the minimal set that encloses all the entries in entryvec
*/
ArrayType *      
_int_common_union(bytea *entryvec, int *sizep, formarray unionf) {
    int numranges, i;
    ArrayType *out = (ArrayType *)NULL;
    ArrayType *tmp;
#ifdef GIST_DEBUG
    elog(NOTICE, "_int_common_union in");
#endif
 
    numranges = (VARSIZE(entryvec) - VARHDRSZ)/sizeof(GISTENTRY); 
    tmp = (ArrayType *)(((GISTENTRY *)(VARDATA(entryvec)))[0]).pred;

    for (i = 1; i < numranges; i++) {
	out = (*unionf)(tmp, (ArrayType *)
				 (((GISTENTRY *)(VARDATA(entryvec)))[i]).pred);
	if (i > 1 && tmp) pfree(tmp);
	tmp = out;
    }

    *sizep = VARSIZE( out );
    if ( *sizep == 0 ) {
	pfree( out );
#ifdef GIST_DEBUG
    elog(NOTICE, "_int_common_union out1");
#endif
	return NULL;
    }
#ifdef GIST_DEBUG
    elog(NOTICE, "_int_common_union out");
#endif
    return(out);

}

/*****************************************
 * The GiST Penalty method for _intments *
 *****************************************/

float *
_int_common_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result,
		formarray unionf,
		formfloat sizef)
{
    Datum ud;
    float tmp1, tmp2;
    
#ifdef GIST_DEBUG
    elog(NOTICE, "penalty");
#endif
    ud = (Datum)(*unionf)((ArrayType *)(origentry->pred), (ArrayType *)(newentry->pred));
    (*sizef)((ArrayType *)ud, &tmp1);
    (*sizef)((ArrayType *)(origentry->pred), &tmp2);
    *result = tmp1 - tmp2;
    pfree((char *)ud);

#ifdef GIST_DEBUG
    elog(NOTICE, "--penalty\t%g", *result);
#endif

    return(result);
}

/*
** The GiST PickSplit method for _intments
** We use Guttman's poly time split algorithm 
*/
GIST_SPLITVEC *
_int_common_picksplit(bytea *entryvec,
	      	GIST_SPLITVEC *v,
		formarray unionf,
		formarray interf,
		formfloat sizef,
		float coef)
{
    OffsetNumber i, j;
    ArrayType *datum_alpha, *datum_beta;
    ArrayType *datum_l, *datum_r;
    ArrayType *union_d, *union_dl, *union_dr;
    ArrayType *inter_d;
    bool firsttime;
    float size_alpha, size_beta, size_union, size_inter;
    float size_waste, waste;
    float size_l, size_r;
    int nbytes;
    OffsetNumber seed_1 = 0, seed_2 = 0;
    OffsetNumber *left, *right;
    OffsetNumber maxoff;

#ifdef GIST_DEBUG
    elog(NOTICE, "--------picksplit %d",(VARSIZE(entryvec) - VARHDRSZ)/sizeof(GISTENTRY));
#endif

    maxoff = ((VARSIZE(entryvec) - VARHDRSZ)/sizeof(GISTENTRY)) - 2;
    nbytes =  (maxoff + 2) * sizeof(OffsetNumber);
    v->spl_left = (OffsetNumber *) palloc(nbytes);
    v->spl_right = (OffsetNumber *) palloc(nbytes);
    
    firsttime = true;
    waste = 0.0;
   
    for (i = FirstOffsetNumber; i < maxoff; i = OffsetNumberNext(i)) {
	datum_alpha = (ArrayType *)(((GISTENTRY *)(VARDATA(entryvec)))[i].pred);
	for (j = OffsetNumberNext(i); j <= maxoff; j = OffsetNumberNext(j)) {
	    datum_beta = (ArrayType *)(((GISTENTRY *)(VARDATA(entryvec)))[j].pred);
	    
	    /* compute the wasted space by unioning these guys */
	    /* size_waste = size_union - size_inter; */
	    union_d = (*unionf)(datum_alpha, datum_beta);
	    (*sizef)(union_d, &size_union);
	    inter_d = (*interf)(datum_alpha, datum_beta);
	    (*sizef)(inter_d, &size_inter);
	    size_waste = size_union - size_inter;
	    
	    pfree(union_d);
	    
	    if (inter_d != (ArrayType *) NULL)
		pfree(inter_d);
	    
	    /*
	     *  are these a more promising split that what we've
	     *  already seen?
	     */
	    
	    if (size_waste > waste || firsttime) {
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
  
    datum_alpha = (ArrayType *)(((GISTENTRY *)(VARDATA(entryvec)))[seed_1].pred);
    datum_l     = copy_intArrayType( datum_alpha ); 
    (*sizef)((ArrayType *)datum_l, &size_l);
    datum_beta  = (ArrayType *)(((GISTENTRY *)(VARDATA(entryvec)))[seed_2].pred);
    datum_r     = copy_intArrayType( datum_beta  ); 
    (*sizef)((ArrayType *)datum_r, &size_r);
    
    /*
     *  Now split up the regions between the two seeds.  An important
     *  property of this split algorithm is that the split vector v
     *  has the indices of items to be split in order in its left and
     *  right vectors.  We exploit this property by doing a merge in
     *  the code that actually splits the page.
     *
     *  For efficiency, we also place the new index tuple in this loop.
     *  This is handled at the very end, when we have placed all the
     *  existing tuples and i == maxoff + 1.
     */
    
    maxoff = OffsetNumberNext(maxoff);
    for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {

	
	/*
	 *  If we've already decided where to place this item, just
	 *  put it on the right list.  Otherwise, we need to figure
	 *  out which page needs the least enlargement in order to
	 *  store the item.
	 */
	
	if (i == seed_1) {
	    *left++ = i;
	    v->spl_nleft++;
	    continue;
	} else if (i == seed_2) {
	    *right++ = i;
	    v->spl_nright++;
	    continue;
	}
	
	/* okay, which page needs least enlargement? */ 
	datum_alpha = (ArrayType *)(((GISTENTRY *)(VARDATA(entryvec)))[i].pred);
	union_dl = (ArrayType *)(*unionf)(datum_l, datum_alpha);
	union_dr = (ArrayType *)(*unionf)(datum_r, datum_alpha);
	(*sizef)((ArrayType *)union_dl, &size_alpha);
	(*sizef)((ArrayType *)union_dr, &size_beta);

	/* pick which page to add it to */
	if (size_alpha - size_l < size_beta - size_r + WISH_F(v->spl_nleft, v->spl_nright, coef)) {
	    if ( datum_l ) pfree(datum_l);
	    if ( union_dr ) pfree(union_dr);
	    datum_l = union_dl;
	    size_l = size_alpha;
	    *left++ = i;
	    v->spl_nleft++;
	} else {
	    if ( datum_r ) pfree(datum_r);
	    if ( union_dl ) pfree(union_dl);
	    datum_r = union_dr;
	    size_r = size_beta;
	    *right++ = i;
	    v->spl_nright++;
	}
    }

    if ( *(left-1) > *(right-1) ) { 
        *right = FirstOffsetNumber;
        *(left-1) = InvalidOffsetNumber;
    } else {
        *left = FirstOffsetNumber;
        *(right-1) = InvalidOffsetNumber;
    }

    v->spl_ldatum = (char *)datum_l;
    v->spl_rdatum = (char *)datum_r;

#ifdef GIST_DEBUG
    elog(NOTICE, "--------ENDpicksplit %d %d",v->spl_nleft, v->spl_nright);
#endif
    return v;
}
