/*-------------------------------------------------------------------------
 *
 * rtree_gist.c
 *    pg_amproc entries for GiSTs over 2-D boxes.
 * This gives R-tree behavior, with Guttman's poly-time split algorithm.
 *
 *
 *
 * IDENTIFICATION
 *	$Header: /cvsroot/pgsql/contrib/rtree_gist/Attic/rtree_gist.c,v 1.1 2001/05/31 18:27:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/itup.h"
#include "access/rtree.h"
#include "utils/palloc.h"
#include "utils/geo_decls.h"
#include "utils/elog.h"

typedef Datum (*RDF)(PG_FUNCTION_ARGS);
typedef Datum (*BINARY_UNION)(Datum, Datum, int*);
typedef float (*SIZE_BOX)(Datum);

/*
** Workaround for index_formtuple
*/
typedef struct polykey {
	int32 size; /* size in varlena terms */ 
	BOX	key;
} POLYKEY; 

/*
** box ops
*/
PG_FUNCTION_INFO_V1(gbox_compress);
PG_FUNCTION_INFO_V1(gbox_union);
PG_FUNCTION_INFO_V1(gbox_picksplit);
PG_FUNCTION_INFO_V1(gbox_consistent);
PG_FUNCTION_INFO_V1(gbox_penalty);
PG_FUNCTION_INFO_V1(gbox_same);

GISTENTRY * gbox_compress(PG_FUNCTION_ARGS);
BOX *gbox_union(PG_FUNCTION_ARGS);
GIST_SPLITVEC * gbox_picksplit(PG_FUNCTION_ARGS);
bool gbox_consistent(PG_FUNCTION_ARGS);
float * gbox_penalty(PG_FUNCTION_ARGS);
bool * gbox_same(PG_FUNCTION_ARGS);

static Datum gbox_binary_union(Datum r1, Datum r2, int *sizep);
static bool gbox_leaf_consistent(BOX *key, BOX *query, StrategyNumber strategy);
static float size_box( Datum box );

/*
** Polygon ops 
*/
PG_FUNCTION_INFO_V1(gpoly_compress);
PG_FUNCTION_INFO_V1(gpoly_union);
PG_FUNCTION_INFO_V1(gpoly_picksplit);
PG_FUNCTION_INFO_V1(gpoly_consistent);
PG_FUNCTION_INFO_V1(gpoly_penalty);
PG_FUNCTION_INFO_V1(gpoly_same);

GISTENTRY * gpoly_compress(PG_FUNCTION_ARGS);
POLYKEY *gpoly_union(PG_FUNCTION_ARGS);
GIST_SPLITVEC * gpoly_picksplit(PG_FUNCTION_ARGS);
bool gpoly_consistent(PG_FUNCTION_ARGS);
float * gpoly_penalty(PG_FUNCTION_ARGS);
bool * gpoly_same(PG_FUNCTION_ARGS);

static Datum gpoly_binary_union(Datum r1, Datum r2, int *sizep);
static float size_polykey( Datum pk );

PG_FUNCTION_INFO_V1(gpoly_inter);
Datum gpoly_inter(PG_FUNCTION_ARGS);

/*
** Common rtree-function (for all ops)
*/
static Datum rtree_union(bytea *entryvec, int *sizep, BINARY_UNION bu);
static float * rtree_penalty(GISTENTRY *origentry, GISTENTRY *newentry, 
		float *result, BINARY_UNION bu, SIZE_BOX sb);
static GIST_SPLITVEC * rtree_picksplit(bytea *entryvec, GIST_SPLITVEC *v, 
	int keylen, BINARY_UNION bu, RDF interop, SIZE_BOX sb);
static bool rtree_internal_consistent(BOX *key, BOX *query, StrategyNumber strategy);

PG_FUNCTION_INFO_V1(rtree_decompress);
GISTENTRY * rtree_decompress(PG_FUNCTION_ARGS);

/**************************************************
 * Box ops
 **************************************************/

/*
** The GiST Consistent method for boxes
** Should return false if for all data items x below entry,
** the predicate x op query == FALSE, where op is the oper
** corresponding to strategy in the pg_amop table.
*/
bool 
gbox_consistent(PG_FUNCTION_ARGS)
{
    GISTENTRY *entry = (GISTENTRY*) PG_GETARG_POINTER(0);
    BOX *query       = (BOX*)	    PG_GETARG_POINTER(1);
    StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
    /*
    ** if entry is not leaf, use gbox_internal_consistent,
    ** else use gbox_leaf_consistent
    */
    if ( ! (DatumGetPointer(entry->key) != NULL && query) )
		return FALSE;

    if (GIST_LEAF(entry))
      PG_RETURN_BOOL(gbox_leaf_consistent((BOX *) DatumGetPointer(entry->key), query, strategy));
    else
      PG_RETURN_BOOL(rtree_internal_consistent((BOX *) DatumGetPointer(entry->key), query, strategy));
}


/*
** The GiST Union method for boxes
** returns the minimal bounding box that encloses all the entries in entryvec
*/
BOX *
gbox_union(PG_FUNCTION_ARGS)
{
    return (BOX*) 
		DatumGetPointer(rtree_union(
			(bytea*) PG_GETARG_POINTER(0),
			(int*) PG_GETARG_POINTER(1),
			gbox_binary_union
			));
}

/*
** GiST Compress methods for boxes
** do not do anything.
*/
GISTENTRY *
gbox_compress(PG_FUNCTION_ARGS)
{
    return((GISTENTRY*)PG_GETARG_POINTER(0));
}

/*
** The GiST Penalty method for boxes
** As in the R-tree paper, we use change in area as our penalty metric
*/
float *
gbox_penalty(PG_FUNCTION_ARGS)
{
    return rtree_penalty(
	(GISTENTRY*) PG_GETARG_POINTER(0),
	(GISTENTRY*) PG_GETARG_POINTER(1),
	(float*) PG_GETARG_POINTER(2),
	gbox_binary_union,
	size_box
    );
}



/*
** The GiST PickSplit method for boxes
** We use Guttman's poly time split algorithm 
*/
GIST_SPLITVEC *
gbox_picksplit(PG_FUNCTION_ARGS)
{
    return rtree_picksplit(
	(bytea*)PG_GETARG_POINTER(0),
	(GIST_SPLITVEC*)PG_GETARG_POINTER(1),
	sizeof(BOX),
	gbox_binary_union,
	rt_box_inter,
	size_box
    );
}

/*
** Equality method
*/
bool *
gbox_same(PG_FUNCTION_ARGS)
{
  BOX *b1 = (BOX*) PG_GETARG_POINTER(0);
  BOX *b2 = (BOX*) PG_GETARG_POINTER(1);
  bool *result = (bool*) PG_GETARG_POINTER(2);
  if ( b1 && b2 )
   	*result = DatumGetBool( DirectFunctionCall2( box_same, PointerGetDatum(b1), PointerGetDatum(b2)) );
  else
	*result = ( b1==NULL && b2==NULL ) ? TRUE : FALSE; 
  return(result);
}

/* 
** SUPPORT ROUTINES for boxes
*/
static bool 
gbox_leaf_consistent(BOX *key,
		     BOX *query,
		     StrategyNumber strategy)
{
    bool retval;

    switch(strategy) {
    case RTLeftStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_left, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    case RTOverLeftStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_overleft, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    case RTOverlapStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_overlap, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    case RTOverRightStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_overright, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    case RTRightStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_right, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    case RTSameStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_same, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    case RTContainsStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_contain, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    case RTContainedByStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_contained, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    default:
      retval = FALSE;
    }
    return(retval);
}

static Datum
gbox_binary_union(Datum r1, Datum r2, int *sizep)
{
    BOX *retval;

    if ( ! (DatumGetPointer(r1) != NULL && DatumGetPointer(r2) != NULL) ) {
		if ( DatumGetPointer(r1) != NULL ) {
			retval = (BOX*) palloc( sizeof(BOX) );
			memcpy( retval, DatumGetPointer(r1), sizeof(BOX) );
    		*sizep = sizeof(BOX);
		} else if ( DatumGetPointer(r2) != NULL ) {
			retval = (BOX*) palloc( sizeof(BOX) );
			memcpy( retval, DatumGetPointer(r2), sizeof(BOX) );
    		*sizep = sizeof(BOX);
		} else {
			*sizep = 0;
			retval = NULL;
		} 
    } else {
    	retval = (BOX*) DatumGetPointer(
			DirectFunctionCall2(rt_box_union, r1, r2));
    	*sizep = sizeof(BOX);
    }
    return PointerGetDatum(retval);
}

static float 
size_box( Datum box ) {
    if ( DatumGetPointer(box) != NULL ) {
		float size;

    	DirectFunctionCall2( rt_box_size,
							 box, PointerGetDatum( &size ) );
		return size;
    } else
		return 0.0;
}

/**************************************************
 * Polygon ops
 **************************************************/

GISTENTRY *
gpoly_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY *entry=(GISTENTRY*)PG_GETARG_POINTER(0);
	GISTENTRY *retval;

	if ( entry->leafkey) {
		retval = palloc(sizeof(GISTENTRY));
		if ( DatumGetPointer(entry->key) != NULL ) {
			POLYGON *in;
			POLYKEY *r;
			in = (POLYGON *) PG_DETOAST_DATUM(entry->key);
			r = (POLYKEY *) palloc( sizeof(POLYKEY) );
			r->size = sizeof(POLYKEY);
			memcpy( (void*)&(r->key), (void*)&(in->boundbox), sizeof(BOX) );
			if ( in != (POLYGON *) DatumGetPointer(entry->key) )
				pfree( in );

			gistentryinit(*retval, PointerGetDatum(r),
						  entry->rel, entry->page,
						  entry->offset, sizeof(POLYKEY), FALSE);

		} else {
			gistentryinit(*retval, (Datum) 0,
						  entry->rel, entry->page,
						  entry->offset, 0,FALSE);
		} 
	} else {
		retval = entry;
	}
	return( retval );
}

bool 
gpoly_consistent(PG_FUNCTION_ARGS)
{
    GISTENTRY *entry = (GISTENTRY*) PG_GETARG_POINTER(0);
    POLYGON *query       = (POLYGON*)PG_DETOAST_DATUM( PG_GETARG_POINTER(1) );
    StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
    bool result;
    /*
    ** if entry is not leaf, use gbox_internal_consistent,
    ** else use gbox_leaf_consistent
    */
    if ( ! (DatumGetPointer(entry->key) != NULL && query) )
	return FALSE;

    result = rtree_internal_consistent((BOX*)&( ((POLYKEY *) DatumGetPointer(entry->key))->key ), 
		&(query->boundbox), strategy);

    PG_FREE_IF_COPY(query,1);
    PG_RETURN_BOOL( result ); 
}

POLYKEY *
gpoly_union(PG_FUNCTION_ARGS)
{
    return (POLYKEY*) 
		DatumGetPointer(rtree_union(
			(bytea*) PG_GETARG_POINTER(0),
			(int*) PG_GETARG_POINTER(1),
			gpoly_binary_union
			));
}

float *
gpoly_penalty(PG_FUNCTION_ARGS)
{
    return rtree_penalty(
	(GISTENTRY*) PG_GETARG_POINTER(0),
	(GISTENTRY*) PG_GETARG_POINTER(1),
	(float*) PG_GETARG_POINTER(2),
	gpoly_binary_union,
	size_polykey
    );
}

GIST_SPLITVEC *
gpoly_picksplit(PG_FUNCTION_ARGS)
{
    return rtree_picksplit(
	(bytea*)PG_GETARG_POINTER(0),
	(GIST_SPLITVEC*)PG_GETARG_POINTER(1),
	sizeof(POLYKEY),
	gpoly_binary_union,
	gpoly_inter,
	size_polykey
    );
}

bool *
gpoly_same(PG_FUNCTION_ARGS)
{
  POLYKEY *b1 = (POLYKEY*) PG_GETARG_POINTER(0);
  POLYKEY *b2 = (POLYKEY*) PG_GETARG_POINTER(1);

  bool *result = (bool*) PG_GETARG_POINTER(2);
  if ( b1 && b2 )
   	*result = DatumGetBool( DirectFunctionCall2( box_same, 
		PointerGetDatum(&(b1->key)), 
		PointerGetDatum(&(b2->key))) );
  else
	*result = ( b1==NULL && b2==NULL ) ? TRUE : FALSE; 
  return(result);
}

/* 
** SUPPORT ROUTINES for polygons
*/
Datum 
gpoly_inter(PG_FUNCTION_ARGS)
{
  	POLYKEY *b1 = (POLYKEY*) PG_GETARG_POINTER(0);
  	POLYKEY *b2 = (POLYKEY*) PG_GETARG_POINTER(1);
	Datum interd;

	interd = DirectFunctionCall2(rt_box_inter,
								 PointerGetDatum( &(b1->key) ),
								 PointerGetDatum( &(b2->key) ));
	
	if (DatumGetPointer(interd) != NULL) {
		POLYKEY *tmp = (POLYKEY*) palloc( sizeof(POLYKEY) );
		tmp->size = sizeof(POLYKEY);
		memcpy( &(tmp->key), DatumGetPointer(interd), sizeof(BOX) );
		pfree( DatumGetPointer(interd) );
		PG_RETURN_POINTER( tmp );
	} else 
		PG_RETURN_POINTER( NULL );
}

static Datum
gpoly_binary_union(Datum r1, Datum r2, int *sizep)
{
    POLYKEY *retval;

    if ( ! (DatumGetPointer(r1) != NULL && DatumGetPointer(r2) != NULL) ) {
		if ( DatumGetPointer(r1) != NULL ) {
			retval = (POLYKEY*)palloc( sizeof(POLYKEY) );
			memcpy( (void*)retval, DatumGetPointer(r1), sizeof(POLYKEY) );
    		*sizep = sizeof(POLYKEY);
		} else if ( DatumGetPointer(r2) != NULL ) {
			retval = (POLYKEY*)palloc( sizeof(POLYKEY) );
			memcpy( (void*)retval, DatumGetPointer(r2), sizeof(POLYKEY) );
    		*sizep = sizeof(POLYKEY);
		} else {
			*sizep = 0;
			retval = NULL;
		} 
    } else {
    	BOX *key = (BOX*)DatumGetPointer(
			DirectFunctionCall2(
			rt_box_union,
			PointerGetDatum( &(((POLYKEY*) DatumGetPointer(r1))->key) ),
			PointerGetDatum( &(((POLYKEY*) DatumGetPointer(r2))->key) )) );
		retval = (POLYKEY*)palloc( sizeof(POLYKEY) );
		memcpy( &(retval->key), key, sizeof(BOX) );
		pfree( key );
    	*sizep = retval->size = sizeof(POLYKEY);
    }
    return PointerGetDatum(retval);
}


static float 
size_polykey( Datum pk ) {
    if ( DatumGetPointer(pk) != NULL ) {
		float size;

    	DirectFunctionCall2( rt_box_size,
							 PointerGetDatum( &(((POLYKEY*) DatumGetPointer(pk))->key) ),
							 PointerGetDatum( &size ) );
		return size;
    } else
		return 0.0;
}


/*
** Common rtree-function (for all ops)
*/

static Datum
rtree_union(bytea *entryvec, int *sizep, BINARY_UNION bu)
{
    int numranges, i;
    Datum	out,
			tmp;

    numranges = (VARSIZE(entryvec) - VARHDRSZ)/sizeof(GISTENTRY); 
    tmp = ((GISTENTRY *) VARDATA(entryvec))[0].key;
    out = (Datum) 0;

    for (i = 1; i < numranges; i++) {
		out = (*bu)(tmp,
					((GISTENTRY *) VARDATA(entryvec))[i].key,
					sizep);
		if (i > 1 && DatumGetPointer(tmp) != NULL)
			pfree(DatumGetPointer(tmp));
		tmp = out;
    }

    return(out);
}

static float *
rtree_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result, BINARY_UNION bu, SIZE_BOX sb)
{
    Datum	ud;
    float tmp1;
    int sizep;
   
    ud = (*bu)( origentry->key, newentry->key, &sizep );
    tmp1 = (*sb)( ud ); 
    if (DatumGetPointer(ud) != NULL) pfree(DatumGetPointer(ud));

    *result = tmp1 - (*sb)( origentry->key );
    return(result);
}

/*
** The GiST PickSplit method
** We use Guttman's poly time split algorithm 
*/
static GIST_SPLITVEC *
rtree_picksplit(bytea *entryvec, GIST_SPLITVEC *v, int keylen, BINARY_UNION bu, RDF interop, SIZE_BOX sb)
{
    OffsetNumber i, j;
    Datum datum_alpha, datum_beta;
    Datum datum_l, datum_r;
    Datum union_d, union_dl, union_dr;
    Datum inter_d;
    bool firsttime;
    float size_alpha, size_beta, size_union, size_inter;
    float size_waste, waste;
    float size_l, size_r;
    int nbytes;
    int sizep;
    OffsetNumber seed_1 = 0, seed_2 = 0;
    OffsetNumber *left, *right;
    OffsetNumber maxoff;

    maxoff = ((VARSIZE(entryvec) - VARHDRSZ)/sizeof(GISTENTRY)) - 2;
    nbytes =  (maxoff + 2) * sizeof(OffsetNumber);
    v->spl_left = (OffsetNumber *) palloc(nbytes);
    v->spl_right = (OffsetNumber *) palloc(nbytes);
    
    firsttime = true;
    waste = 0.0;
    
    for (i = FirstOffsetNumber; i < maxoff; i = OffsetNumberNext(i)) {
	datum_alpha = ((GISTENTRY *) VARDATA(entryvec))[i].key;
	for (j = OffsetNumberNext(i); j <= maxoff; j = OffsetNumberNext(j)) {
	    datum_beta = ((GISTENTRY *) VARDATA(entryvec))[j].key;
	    
	    /* compute the wasted space by unioning these guys */
	    /* size_waste = size_union - size_inter; */	
	    union_d = (*bu)( datum_alpha, datum_beta, &sizep );
	    if ( DatumGetPointer(union_d) != NULL ) {
			size_union = (*sb)(union_d);
			pfree(DatumGetPointer(union_d));
	    } else
			size_union = 0.0;

	    if ( DatumGetPointer(datum_alpha) != NULL &&
			 DatumGetPointer(datum_beta) != NULL ) {
			inter_d = DirectFunctionCall2(interop,
										  datum_alpha,
										  datum_beta);
			if ( DatumGetPointer(inter_d) != NULL ) {
				size_inter = (*sb)(inter_d);
				pfree(DatumGetPointer(inter_d));
			} else 
				size_inter = 0.0;
	    } else
			size_inter = 0.0;

	    size_waste = size_union - size_inter;
	    
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
  
    if ( DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[seed_1].key) != NULL )
	{
		datum_l = PointerGetDatum(palloc( keylen ));
		memcpy(DatumGetPointer(datum_l),
			   DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[seed_1].key),
			   keylen);
    } else 
		datum_l = (Datum) 0;
    size_l  = (*sb)( datum_l ); 
    if ( DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[seed_2].key) != NULL )
	{
		datum_r = PointerGetDatum(palloc( keylen ));
		memcpy(DatumGetPointer(datum_r),
			   DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[seed_2].key),
			   keylen);
    } else 
		datum_r = (Datum) 0;
    size_r  = (*sb)( datum_r ); 
    
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
	datum_alpha = ((GISTENTRY *) VARDATA(entryvec))[i].key;
	union_dl = (*bu)( datum_l, datum_alpha, &sizep );
	union_dr = (*bu)( datum_r, datum_alpha, &sizep );
	size_alpha = (*sb)( union_dl ); 
	size_beta  = (*sb)( union_dr ); 
	
	/* pick which page to add it to */
	if (size_alpha - size_l < size_beta - size_r) {
	    pfree(DatumGetPointer(datum_l));
	    pfree(DatumGetPointer(union_dr));
	    datum_l = union_dl;
	    size_l = size_alpha;
	    *left++ = i;
	    v->spl_nleft++;
	} else {
	    pfree(DatumGetPointer(datum_r));
	    pfree(DatumGetPointer(union_dl));
	    datum_r = union_dr;
	    size_r = size_alpha;
	    *right++ = i;
	    v->spl_nright++;
	}
    }
    *left = *right = FirstOffsetNumber;	/* sentinel value, see dosplit() */
    
    v->spl_ldatum = datum_l;
    v->spl_rdatum = datum_r;

    return( v );
}

static bool 
rtree_internal_consistent(BOX *key,
			BOX *query,
			StrategyNumber strategy)
{
    bool retval;

    switch(strategy) {
    case RTLeftStrategyNumber:
    case RTOverLeftStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_overleft, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    case RTOverlapStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_overlap, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    case RTOverRightStrategyNumber:
    case RTRightStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_right, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    case RTSameStrategyNumber:
    case RTContainsStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_contain, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    case RTContainedByStrategyNumber:
      retval = DatumGetBool( DirectFunctionCall2( box_overlap, PointerGetDatum(key), PointerGetDatum(query) ) );
      break;
    default:
      retval = FALSE;
    }
    return(retval);
}

/*
** GiST DeCompress methods
** do not do anything.
*/
GISTENTRY *
rtree_decompress(PG_FUNCTION_ARGS)
{
    return((GISTENTRY*)PG_GETARG_POINTER(0));
}
