/******************************************************************************
  This file contains routines that can be bound to a Postgres backend and
  called by the backend in the process of processing queries.  The calling
  format for these routines is dictated by Postgres architecture.
******************************************************************************/

#include "postgres.h"

#include <math.h>

#include "access/gist.h"
#include "access/rtree.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/builtins.h"

#include "cubedata.h"

#define max(a,b)        ((a) >  (b) ? (a) : (b))
#define min(a,b)        ((a) <= (b) ? (a) : (b))
#define abs(a)          ((a) <  (0) ? (-a) : (a))

extern void  set_parse_buffer(char *str);
extern int   cube_yyparse();

/*
** Input/Output routines
*/
NDBOX *      cube_in(char *str);
char *       cube_out(NDBOX *cube);


/* 
** GiST support methods
*/
bool             g_cube_consistent(GISTENTRY *entry, NDBOX *query, StrategyNumber strategy); 
GISTENTRY *      g_cube_compress(GISTENTRY *entry);
GISTENTRY *      g_cube_decompress(GISTENTRY *entry);
float *          g_cube_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result);
GIST_SPLITVEC *  g_cube_picksplit(bytea *entryvec, GIST_SPLITVEC *v);
bool             g_cube_leaf_consistent(NDBOX *key, NDBOX *query, StrategyNumber strategy);
bool             g_cube_internal_consistent(NDBOX *key, NDBOX *query, StrategyNumber strategy);
NDBOX *          g_cube_union(bytea *entryvec, int *sizep);
NDBOX *          g_cube_binary_union(NDBOX *r1, NDBOX *r2, int *sizep);
bool *           g_cube_same(NDBOX *b1, NDBOX *b2, bool *result);

/*
** R-tree suport functions
*/
bool         cube_same(NDBOX *a, NDBOX *b);
bool         cube_different(NDBOX *a, NDBOX *b);
bool         cube_contains(NDBOX *a, NDBOX *b);
bool         cube_contained (NDBOX *a, NDBOX *b);
bool         cube_overlap(NDBOX *a, NDBOX *b);
NDBOX *      cube_union(NDBOX *a, NDBOX *b);
NDBOX *      cube_inter(NDBOX *a, NDBOX *b);
float *      cube_size(NDBOX *a);
void         rt_cube_size(NDBOX *a, float *sz);

/*
** These make no sense for this type, but R-tree wants them
*/
bool         cube_over_left(NDBOX *a, NDBOX *b);
bool         cube_over_right(NDBOX *a, NDBOX *b);
bool         cube_left(NDBOX *a, NDBOX *b);
bool         cube_right(NDBOX *a, NDBOX *b);

/*
** miscellaneous
*/
bool         cube_lt(NDBOX *a, NDBOX *b);
bool         cube_gt(NDBOX *a, NDBOX *b);
float *      cube_distance(NDBOX *a, NDBOX *b);

/* 
** Auxiliary funxtions
*/
static       float distance_1D(float a1, float a2, float b1, float b2);
static       NDBOX *swap_corners (NDBOX *a);


/*****************************************************************************
 * Input/Output functions
 *****************************************************************************/

/* NdBox = [(lowerleft),(upperright)] */
/* [(xLL(1)...xLL(N)),(xUR(1)...xUR(n))] */
NDBOX *
cube_in(char *str)
{
  void * result;

  set_parse_buffer( str );

  if ( cube_yyparse(&result) != 0 ) {
    return NULL;
  } 

  return ( (NDBOX *)result );
}

/*
 * You might have noticed a slight inconsistency between the following
 * declaration and the SQL definition:
 *     CREATE FUNCTION cube_out(opaque) RETURNS opaque ...
 * The reason is that the argument pass into cube_out is really just a
 * pointer. POSTGRES thinks all output functions are:
 *     char *out_func(char *);
 */
char *
cube_out(NDBOX *cube)
{
    char *result;
    char *p;
    int equal = 1;
    int dim = cube->dim;
    int i;

    if (cube == NULL)
	return(NULL);

    p = result = (char *) palloc(100);

    /* while printing the first (LL) corner, check if it is equal
    to the scond one */
    p += sprintf(p, "(");
    for ( i=0; i < dim; i++ ) {
      p += sprintf(p, "%g", cube->x[i]);
      p += sprintf(p, ", ");
      if ( cube->x[i] != cube->x[i+dim] ) {
	equal = 0;
      }
    }
    p -= 2; /* get rid of the last ", " */
    p += sprintf(p, ")");

    if ( !equal ) {
    p += sprintf(p, ",(");
      for ( i=dim; i < dim * 2; i++ ) {
	p += sprintf(p, "%g", cube->x[i]);
	p += sprintf(p, ", ");
      }
      p -= 2; 
      p += sprintf(p, ")");
    }
      
    return(result);
}


/*****************************************************************************
 *                         GiST functions
 *****************************************************************************/

/*
** The GiST Consistent method for boxes
** Should return false if for all data items x below entry,
** the predicate x op query == FALSE, where op is the oper
** corresponding to strategy in the pg_amop table.
*/
bool 
g_cube_consistent(GISTENTRY *entry,
	       NDBOX *query,
	       StrategyNumber strategy)
{
    /*
    ** if entry is not leaf, use g_cube_internal_consistent,
    ** else use g_cube_leaf_consistent
    */
    if (GIST_LEAF(entry))
      return(g_cube_leaf_consistent((NDBOX *)(entry->pred), query, strategy));
    else
      return(g_cube_internal_consistent((NDBOX *)(entry->pred), query, strategy));
}


/*
** The GiST Union method for boxes
** returns the minimal bounding box that encloses all the entries in entryvec
*/
NDBOX *
g_cube_union(bytea *entryvec, int *sizep)
{
    int numranges, i;
    NDBOX *out = (NDBOX *)NULL;
    NDBOX *tmp;

    /*
    fprintf(stderr, "union\n");
    */
    numranges = (VARSIZE(entryvec) - VARHDRSZ)/sizeof(GISTENTRY); 
    tmp = (NDBOX *)(((GISTENTRY *)(VARDATA(entryvec)))[0]).pred;
    /*
    *sizep = sizeof(NDBOX); -- NDBOX has variable size
    */
    *sizep = tmp->size;

    for (i = 1; i < numranges; i++) {
      out = g_cube_binary_union(tmp, (NDBOX *)
				 (((GISTENTRY *)(VARDATA(entryvec)))[i]).pred,
				 sizep);
      /*
	fprintf(stderr, "\t%s ^ %s -> %s\n", cube_out(tmp), cube_out((NDBOX *)(((GISTENTRY *)(VARDATA(entryvec)))[i]).pred), cube_out(out));
      */
      if (i > 1) pfree(tmp);
      tmp = out;
    }

    return(out);
}

/*
** GiST Compress and Decompress methods for boxes
** do not do anything.
*/
GISTENTRY *
g_cube_compress(GISTENTRY *entry)
{
    return(entry);
}

GISTENTRY *
g_cube_decompress(GISTENTRY *entry)
{
    return(entry);
}

/*
** The GiST Penalty method for boxes
** As in the R-tree paper, we use change in area as our penalty metric
*/
float *
g_cube_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result)
{
    Datum ud;
    float tmp1, tmp2;
    
    ud = (Datum)cube_union((NDBOX *)(origentry->pred), (NDBOX *)(newentry->pred));
    rt_cube_size((NDBOX *)ud, &tmp1);
    rt_cube_size((NDBOX *)(origentry->pred), &tmp2);
    *result = tmp1 - tmp2;
    pfree((char *)ud);
    /*
    fprintf(stderr, "penalty\n");
    fprintf(stderr, "\t%g\n", *result);
    */
    return(result);
}



/*
** The GiST PickSplit method for boxes
** We use Guttman's poly time split algorithm 
*/
GIST_SPLITVEC *
g_cube_picksplit(bytea *entryvec,
	      GIST_SPLITVEC *v)
{
    OffsetNumber i, j;
    NDBOX *datum_alpha, *datum_beta;
    NDBOX *datum_l, *datum_r;
    NDBOX *union_d, *union_dl, *union_dr;
    NDBOX *inter_d;
    bool firsttime;
    float size_alpha, size_beta, size_union, size_inter;
    float size_waste, waste;
    float size_l, size_r;
    int nbytes;
    OffsetNumber seed_1 = 0, seed_2 = 0;
    OffsetNumber *left, *right;
    OffsetNumber maxoff;

    /*
    fprintf(stderr, "picksplit\n");
    */
    maxoff = ((VARSIZE(entryvec) - VARHDRSZ)/sizeof(GISTENTRY)) - 2;
    nbytes =  (maxoff + 2) * sizeof(OffsetNumber);
    v->spl_left = (OffsetNumber *) palloc(nbytes);
    v->spl_right = (OffsetNumber *) palloc(nbytes);
    
    firsttime = true;
    waste = 0.0;
    
    for (i = FirstOffsetNumber; i < maxoff; i = OffsetNumberNext(i)) {
	datum_alpha = (NDBOX *)(((GISTENTRY *)(VARDATA(entryvec)))[i].pred);
	for (j = OffsetNumberNext(i); j <= maxoff; j = OffsetNumberNext(j)) {
	    datum_beta = (NDBOX *)(((GISTENTRY *)(VARDATA(entryvec)))[j].pred);
	    
	    /* compute the wasted space by unioning these guys */
	    /* size_waste = size_union - size_inter; */
	    union_d = (NDBOX *)cube_union(datum_alpha, datum_beta);
	    rt_cube_size(union_d, &size_union);
	    inter_d = (NDBOX *)cube_inter(datum_alpha, datum_beta);
	    rt_cube_size(inter_d, &size_inter);
	    size_waste = size_union - size_inter;
	    
	    pfree(union_d);
	    
	    if (inter_d != (NDBOX *) NULL)
		pfree(inter_d);
	    
	    /*
	     *  are these a more promising split than what we've
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
    
    datum_alpha = (NDBOX *)(((GISTENTRY *)(VARDATA(entryvec)))[seed_1].pred);
    datum_l = (NDBOX *)cube_union(datum_alpha, datum_alpha);
    rt_cube_size((NDBOX *)datum_l, &size_l);
    datum_beta = (NDBOX *)(((GISTENTRY *)(VARDATA(entryvec)))[seed_2].pred);;
    datum_r = (NDBOX *)cube_union(datum_beta, datum_beta);
    rt_cube_size((NDBOX *)datum_r, &size_r);
    
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
	datum_alpha = (NDBOX *)(((GISTENTRY *)(VARDATA(entryvec)))[i].pred);
	union_dl = (NDBOX *)cube_union(datum_l, datum_alpha);
	union_dr = (NDBOX *)cube_union(datum_r, datum_alpha);
	rt_cube_size((NDBOX *)union_dl, &size_alpha);
	rt_cube_size((NDBOX *)union_dr, &size_beta);
	
	/* pick which page to add it to */
	if (size_alpha - size_l < size_beta - size_r) {
	    pfree(datum_l);
	    pfree(union_dr);
	    datum_l = union_dl;
	    size_l = size_alpha;
	    *left++ = i;
	    v->spl_nleft++;
	} else {
	    pfree(datum_r);
	    pfree(union_dl);
	    datum_r = union_dr;
	    size_r = size_alpha;
	    *right++ = i;
	    v->spl_nright++;
	}
    }
    *left = *right = FirstOffsetNumber;	/* sentinel value, see dosplit() */
    
    v->spl_ldatum = (char *)datum_l;
    v->spl_rdatum = (char *)datum_r;

    return v;
}

/*
** Equality method
*/
bool *
g_cube_same(NDBOX *b1, NDBOX *b2, bool *result)
{
  if (cube_same(b1, b2))
    *result = TRUE;
  else *result = FALSE;
  /*
  fprintf(stderr, "same: %s\n", (*result ? "TRUE" : "FALSE" ));
  */
  return(result);
}

/* 
** SUPPORT ROUTINES
*/
bool 
g_cube_leaf_consistent(NDBOX *key,
		     NDBOX *query,
		     StrategyNumber strategy)
{
  bool retval;

  /*
  fprintf(stderr, "leaf_consistent, %d\n", strategy);
  */
  switch(strategy) {
  case RTLeftStrategyNumber:
    retval = (bool)cube_left(key, query);
    break;
  case RTOverLeftStrategyNumber:
    retval = (bool)cube_over_left(key,query);
    break;
  case RTOverlapStrategyNumber:
    retval = (bool)cube_overlap(key, query);
    break;
  case RTOverRightStrategyNumber:
    retval = (bool)cube_over_right(key, query);
    break;
  case RTRightStrategyNumber:
    retval = (bool)cube_right(key, query);
    break;
  case RTSameStrategyNumber:
    retval = (bool)cube_same(key, query);
    break;
  case RTContainsStrategyNumber:
    retval = (bool)cube_contains(key, query);
    break;
  case RTContainedByStrategyNumber:
    retval = (bool)cube_contained(key,query);
    break;
  default:
    retval = FALSE;
  }
  return(retval);
}

bool 
g_cube_internal_consistent(NDBOX *key,
			NDBOX *query,
			StrategyNumber strategy)
{
  bool retval;
  
  /*
  fprintf(stderr, "internal_consistent, %d\n", strategy);
  */
  switch(strategy) {
  case RTLeftStrategyNumber:
  case RTOverLeftStrategyNumber:
    retval = (bool)cube_over_left(key,query);
    break;
  case RTOverlapStrategyNumber:
    retval = (bool)cube_overlap(key, query);
    break;
  case RTOverRightStrategyNumber:
  case RTRightStrategyNumber:
    retval = (bool)cube_right(key, query);
    break;
  case RTSameStrategyNumber:
  case RTContainsStrategyNumber:
    retval = (bool)cube_contains(key, query);
    break;
  case RTContainedByStrategyNumber:
    retval = (bool)cube_overlap(key, query);
    break;
  default:
    retval = FALSE;
    }
  return(retval);
}

NDBOX *
g_cube_binary_union(NDBOX *r1, NDBOX *r2, int *sizep)
{
    NDBOX *retval;

    retval = cube_union(r1, r2);
    *sizep = retval->size;

    return (retval);
}


/* cube_union */
NDBOX *cube_union(NDBOX *box_a, NDBOX *box_b)
{
  int i;
  NDBOX *result;
  NDBOX *a = swap_corners(box_a);
  NDBOX *b = swap_corners(box_b);

  if ( a->dim >= b->dim ) {
    result = palloc(a->size);
    result->size = a->size;
    result->dim = a->dim;
  }
  else {
    result = palloc(b->size);
    result->size = b->size;
    result->dim = b->dim;
  }

  /* swap the box pointers if needed */
  if ( a->dim < b->dim ) {
    NDBOX * tmp = b; b = a; a = tmp;
  }

  /* use the potentially smaller of the two boxes (b) to fill in 
     the result, padding absent dimensions with zeroes*/
  for ( i = 0; i < b->dim; i++ ) {
    result->x[i] = b->x[i];
    result->x[i + a->dim] = b->x[i + b->dim];
  }
  for ( i = b->dim; i < a->dim; i++ ) {
    result->x[i] = 0;
    result->x[i + a->dim] = 0;
  }
    
  /* compute the union */
  for ( i = 0; i < a->dim; i++ ) {
    result->x[i] = min(a->x[i], result->x[i]);
  }
  for ( i = a->dim; i < a->dim * 2; i++ ) {
    result->x[i] = max(a->x[i], result->x[i]);
  }

  pfree(a);
  pfree(b);

  return(result);
}

/* cube_inter */
NDBOX *cube_inter(NDBOX *box_a, NDBOX *box_b)
{
  int i;
  NDBOX * result;
  NDBOX *a = swap_corners(box_a);
  NDBOX *b = swap_corners(box_b);
  
  if ( a->dim >= b->dim ) {
    result = palloc(a->size);
    result->size = a->size;
    result->dim = a->dim;
  }
  else {
    result = palloc(b->size);
    result->size = b->size;
    result->dim = b->dim;
  }

  /* swap the box pointers if needed */
  if ( a->dim < b->dim ) {
    NDBOX * tmp = b; b = a; a = tmp;
  }

  /* use the potentially  smaller of the two boxes (b) to fill in 
     the result, padding absent dimensions with zeroes*/
  for ( i = 0; i < b->dim; i++ ) {
    result->x[i] = b->x[i];
    result->x[i + a->dim] = b->x[i + b->dim];
  }
  for ( i = b->dim; i < a->dim; i++ ) {
    result->x[i] = 0;
    result->x[i + a->dim] = 0;
  }
    
  /* compute the intersection */
  for ( i = 0; i < a->dim; i++ ) {
    result->x[i] = max(a->x[i], result->x[i]);
  }
  for ( i = a->dim; i < a->dim * 2; i++ ) {
    result->x[i] = min(a->x[i], result->x[i]);
  }
  
  pfree(a);
  pfree(b);

  /* Is it OK to return a non-null intersection for non-overlapping boxes? */
  return(result);
}

/* cube_size */
float *cube_size(NDBOX *a)
{
  int i,j;
  float *result;

  result = (float *) palloc(sizeof(float));
  
  *result = 1.0;
  for ( i = 0, j = a->dim; i < a->dim; i++,j++ ) {
    *result=(*result)*abs((a->x[j] - a->x[i]));
  }
  
  return(result);
}

void
rt_cube_size(NDBOX *a, float *size)
{
  int i,j;
  if (a == (NDBOX *) NULL)
    *size = 0.0;
  else {
    *size = 1.0;
    for ( i = 0, j = a->dim; i < a->dim; i++,j++ ) {
      *size=(*size)*abs((a->x[j] - a->x[i]));
    }
  }
  return;
}

/* The following four methods compare the projections of the boxes
   onto the 0-th coordinate axis. These methods are useless for dimensions
   larger than 2, but it seems that R-tree requires all its strategies
   map to real functions that return something */

/*  is the right edge of (a) located to the left of 
    the right edge of (b)? */
bool cube_over_left(NDBOX *box_a, NDBOX *box_b)
{
  NDBOX *a;
  NDBOX *b;
  
  if ( (box_a==NULL) || (box_b==NULL) )
    return(FALSE);

  a = swap_corners(box_a);
  b = swap_corners(box_b);

  return( a->x[a->dim - 1] <= b->x[b->dim - 1] && !cube_left(a, b) && !cube_right(a, b) );
}

/*  is the left edge of (a) located to the right of 
    the left edge of (b)? */
bool cube_over_right(NDBOX *box_a, NDBOX *box_b)
{
  NDBOX *a;
  NDBOX *b;
  
  if ( (box_a==NULL) || (box_b==NULL) )
    return(FALSE);

  a = swap_corners(box_a);
  b = swap_corners(box_b);

  return( a->x[a->dim - 1] >= b->x[b->dim - 1] && !cube_left(a, b) && !cube_right(a, b) );
}


/* return 'true' if the projection of 'a' is
   entirely on the left of the projection of 'b' */
bool cube_left(NDBOX *box_a, NDBOX *box_b)
{
  NDBOX *a;
  NDBOX *b;
  
  if ( (box_a==NULL) || (box_b==NULL) )
    return(FALSE);

  a = swap_corners(box_a);
  b = swap_corners(box_b);

  return( a->x[a->dim - 1] < b->x[0]);
}

/* return 'true' if the projection of 'a' is
   entirely on the right  of the projection of 'b' */
bool cube_right(NDBOX *box_a, NDBOX *box_b)
{
  NDBOX *a;
  NDBOX *b;
  
  if ( (box_a==NULL) || (box_b==NULL) )
    return(FALSE);

  a = swap_corners(box_a);
  b = swap_corners(box_b);

  return( a->x[0] > b->x[b->dim - 1]);
}

/* make up a metric in which one box will be 'lower' than the other
   -- this can be useful for srting and to determine uniqueness */
bool cube_lt(NDBOX *box_a, NDBOX *box_b)
{
  int i;
  int dim;
  NDBOX *a;
  NDBOX *b;
  
  if ( (box_a==NULL) || (box_b==NULL) )
    return(FALSE);

  a = swap_corners(box_a);
  b = swap_corners(box_b);
  dim = min(a->dim, b->dim);

  /* if all common dimensions are equal, the cube with more dimensions wins */
  if ( cube_same(a, b) ) {
    if (a->dim < b->dim) {
      return(TRUE);
    }
    else {
      return(FALSE);
    }
  }

  /* compare the common dimensions */
  for ( i = 0; i < dim; i++ ) {
    if ( a->x[i] > b->x[i] )
      return(FALSE); 
    if ( a->x[i] < b->x[i] )
      return(TRUE); 
  }
  for ( i = 0; i < dim; i++ ) {
    if ( a->x[i + a->dim] > b->x[i + b->dim] )
      return(FALSE); 
    if ( a->x[i + a->dim] < b->x[i + b->dim] )
      return(TRUE); 
  }

  /* compare extra dimensions to zero */
  if ( a->dim > b->dim ) {
    for ( i = dim; i < a->dim; i++ ) {
      if ( a->x[i] > 0 )
	return(FALSE); 
      if ( a->x[i] < 0 )
	return(TRUE); 
    }
    for ( i = 0; i < dim; i++ ) {
      if ( a->x[i + a->dim] > 0 )
	return(FALSE); 
      if ( a->x[i + a->dim] < 0 )
	return(TRUE); 
    }
  }
  if ( a->dim < b->dim ) {
    for ( i = dim; i < b->dim; i++ ) {
      if ( b->x[i] > 0 )
	return(TRUE); 
      if ( b->x[i] < 0 )
	return(FALSE); 
    }
    for ( i = 0; i < dim; i++ ) {
      if ( b->x[i + b->dim] > 0 )
	return(TRUE); 
      if ( b->x[i + b->dim] < 0 )
	return(FALSE);
    }
  }
    
  return(FALSE);
}


bool cube_gt(NDBOX *box_a, NDBOX *box_b)
{
  int i;
  int dim;
  NDBOX *a;
  NDBOX *b;
  
  if ( (box_a==NULL) || (box_b==NULL) )
    return(FALSE);

  a = swap_corners(box_a);
  b = swap_corners(box_b);
  dim = min(a->dim, b->dim);

  /* if all common dimensions are equal, the cube with more dimensions wins */
  if ( cube_same(a, b) ) {
    if (a->dim > b->dim) {
      return(TRUE);
    }
    else {
      return(FALSE);
    }
  }

  /* compare the common dimensions */
  for ( i = 0; i < dim; i++ ) {
    if ( a->x[i] < b->x[i] )
      return(FALSE); 
    if ( a->x[i] > b->x[i] )
      return(TRUE); 
  }
  for ( i = 0; i < dim; i++ ) {
    if ( a->x[i + a->dim] < b->x[i + b->dim] )
      return(FALSE); 
    if ( a->x[i + a->dim] > b->x[i + b->dim] )
      return(TRUE); 
  }


  /* compare extra dimensions to zero */
  if ( a->dim > b->dim ) {
    for ( i = dim; i < a->dim; i++ ) {
      if ( a->x[i] < 0 )
	return(FALSE); 
      if ( a->x[i] > 0 )
	return(TRUE); 
    }
    for ( i = 0; i < dim; i++ ) {
      if ( a->x[i + a->dim] < 0 )
	return(FALSE); 
      if ( a->x[i + a->dim] > 0 )
	return(TRUE); 
    }
  }
  if ( a->dim < b->dim ) {
    for ( i = dim; i < b->dim; i++ ) {
      if ( b->x[i] < 0 )
	return(TRUE); 
      if ( b->x[i] > 0 )
	return(FALSE); 
    }
    for ( i = 0; i < dim; i++ ) {
      if ( b->x[i + b->dim] < 0 )
	return(TRUE); 
      if ( b->x[i + b->dim] > 0 )
	return(FALSE);
    }
  }

  return(FALSE);
}


/* Equal */
bool cube_same(NDBOX *box_a, NDBOX *box_b)
{
  int i;
  NDBOX *a;
  NDBOX *b;
  
  if ( (box_a==NULL) || (box_b==NULL) )
    return(FALSE);

  a = swap_corners(box_a);
  b = swap_corners(box_b);

  /* swap the box pointers if necessary */
  if ( a->dim < b->dim ) {
    NDBOX * tmp = b; b = a; a = tmp;
  }

  for ( i = 0; i < b->dim; i++ ) {
    if ( a->x[i] != b->x[i] )
      return(FALSE);
    if ( a->x[i + a->dim] != b->x[i + b->dim] )
      return(FALSE);
  }

  /* all dimensions of (b) are compared to those of (a);
     instead of those in (a) absent in (b), compare (a) to zero */
  for ( i = b->dim; i < a->dim; i++ ) {
     if ( a->x[i] != 0 )
      return(FALSE);
     if ( a->x[i + a->dim] != 0 )
      return(FALSE);
  }

  pfree(a);
  pfree(b);

  return(TRUE);
}

/* Different */
bool cube_different(NDBOX *box_a, NDBOX *box_b)
{
  return(!cube_same(box_a, box_b));
}


/* Contains */
/* Box(A) CONTAINS Box(B) IFF pt(A) < pt(B) */
bool cube_contains(NDBOX *box_a, NDBOX *box_b)
{
  int i;
  NDBOX *a;
  NDBOX *b;

  if ( (box_a==NULL) || (box_b==NULL) )
    return(FALSE);

  a = swap_corners(box_a);
  b = swap_corners(box_b);

  if ( a->dim < b->dim ) {
    /* the further comparisons will make sense if the 
       excess dimensions of (b) were zeroes */
    for ( i = a->dim; i < b->dim; i++ ) {
      if ( b->x[i] != 0 )
	return(FALSE);
      if ( b->x[i + b->dim] != 0 )
	return(FALSE);
    }
  }

  /* Can't care less about the excess dimensions of (a), if any */
  for ( i = 0; i < min(a->dim, b->dim); i++ ) {
    if ( a->x[i] > b->x[i] )
      return(FALSE);
    if ( a->x[i + a->dim] < b->x[i + b->dim] )
      return(FALSE);
  }

  pfree(a);
  pfree(b);

  return(TRUE);
}

/* Contained */
/* Box(A) Contained by Box(B) IFF Box(B) Contains Box(A) */
bool cube_contained (NDBOX *a, NDBOX *b)
{
  if (cube_contains(b,a) == TRUE)
    return(TRUE);
  else
    return(FALSE);
}

/* Overlap */
/* Box(A) Overlap Box(B) IFF (pt(a)LL < pt(B)UR) && (pt(b)LL < pt(a)UR) */
bool cube_overlap(NDBOX *box_a, NDBOX *box_b)
{
  int i;
  NDBOX *a;
  NDBOX *b;

  /* This *very bad* error was found in the source: 
	if ( (a==NULL) || (b=NULL) )
		return(FALSE);
  */
  if ( (box_a==NULL) || (box_b==NULL) )
    return(FALSE);

  a = swap_corners(box_a);
  b = swap_corners(box_b);

  /* swap the box pointers if needed */
  if ( a->dim < b->dim ) {
    NDBOX * tmp = b; b = a; a = tmp;
  }

  /* compare within the dimensions of (b) */
  for ( i = 0; i < b->dim; i++ ) {
    if ( a->x[i] > b->x[i + b->dim] )
      return(FALSE);
    if ( a->x[i + a->dim] < b->x[i] )
      return(FALSE);
  }

  /* compare to zero those dimensions in (a) absent in (b) */
  for ( i = b->dim; i < a->dim; i++ ) {
     if ( a->x[i] > 0 )
      return(FALSE);
     if ( a->x[i + a->dim] < 0 )
      return(FALSE);
  }

  pfree(a);
  pfree(b);

  return(TRUE);
}


/* Distance */
/* The distance is computed as a per axis sum of the squared distances
   between 1D projections of the boxes onto Cartesian axes. Assuming zero 
   distance between overlapping projections, this metric coincides with the 
   "common sense" geometric distance */
float *cube_distance(NDBOX *a, NDBOX *b)
{
  int i;
  double d, distance;
  float *result;

  result = (float *) palloc(sizeof(float));
  
  /* swap the box pointers if needed */
  if ( a->dim < b->dim ) {
    NDBOX * tmp = b; b = a; a = tmp;
  }

  distance = 0.0;
  /* compute within the dimensions of (b) */
  for ( i = 0; i < b->dim; i++ ) {
    d = distance_1D(a->x[i], a->x[i + a->dim], b->x[i], b->x[i + b->dim]);
    distance += d*d;
  }

  /* compute distance to zero for those dimensions in (a) absent in (b) */
  for ( i = b->dim; i < a->dim; i++ ) {
    d = distance_1D(a->x[i], a->x[i + a->dim], 0.0, 0.0);
    distance += d*d;
  }
  
  *result = (float)sqrt(distance);

  return(result);
}

static float distance_1D(float a1, float a2, float b1, float b2)
{
  /* interval (a) is entirely on the left of (b) */
  if( (a1 <= b1) && (a2 <= b1) && (a1 <= b2) && (a2 <= b2) ) {
    return ( min( b1, b2 ) - max( a1, a2 ) );
  }

  /* interval (a) is entirely on the right of (b) */
  if( (a1 > b1) && (a2 > b1) && (a1 > b2) && (a2 > b2) ) {
    return ( min( a1, a2 ) - max( b1, b2 ) );
  }
  
  /* the rest are all sorts of intersections */
  return(0.0);
}

/* normalize the box's co-ordinates by placing min(xLL,xUR) to LL
   and max(xLL,xUR) to UR 
*/
static NDBOX *swap_corners ( NDBOX *a )
{
  int i, j;
  NDBOX * result;
  
  result = palloc(a->size);
  result->size = a->size;
  result->dim = a->dim;

  for ( i = 0, j = a->dim; i < a->dim; i++, j++ ) {
    result->x[i] = min(a->x[i],a->x[j]);
    result->x[j] = max(a->x[i],a->x[j]);
  }

  return(result);
}
