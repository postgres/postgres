/*-------------------------------------------------------------------------
 *
 * geo_ops.c--
 *    2D geometric operations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/geo_ops.c,v 1.3 1997/04/22 17:31:32 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <float.h>
#include <stdio.h>	/* for sprintf proto, etc. */
#include <stdlib.h>	/* for strtod, etc. */
#include <string.h>
#include <ctype.h>

#include "postgres.h"

#include "utils/geo_decls.h"
#include "utils/palloc.h"

#define OLD_FORMAT_IN	1
#define OLD_FORMAT_OUT	0

/*
 * Delimiters for input and output strings.
 * LDELIM, RDELIM, and DELIM are left, right, and separator delimiters, respectively.
 * LDELIM_EP, RDELIM_EP are left and right delimiters for paths with endpoints.
 */

#define LDELIM		'('
#define RDELIM		')'
#define	DELIM		','
#define LDELIM_EP	'['
#define RDELIM_EP	']'
#define LDELIM_C	'<'
#define RDELIM_C	'>'

/* Maximum number of output digits printed */
#define P_MAXDIG DBL_DIG
#define P_MAXLEN (2*(P_MAXDIG+7)+1)

static int digits8 = P_MAXDIG;

int geo_precision(int digits);

int geo_precision(int digits)
{
    if (digits > P_MAXDIG) {
	digits8 = P_MAXDIG;
    } else if (digits > 0) {
	digits8 = digits;
    };
    return digits8;
}

/*
 * Geometric data types are composed of points.
 * This code tries to support a common format throughout the data types,
 *  to allow for more predictable usage and data type conversion.
 * The fundamental unit is the point. Other units are line segments,
 *  open paths, boxes, closed paths, and polygons (which should be considered
 *  non-intersecting closed paths).
 *
 * Data representation is as follows:
 *  point:		(x,y)
 *  line segment:	[(x1,y1),(x2,y2)]
 *  box:		(x1,y1),(x2,y2)
 *  open path:		[(x1,y1),...,(xn,yn)]
 *  closed path:	((x1,y1),...,(xn,yn))
 *  polygon:		((x1,y1),...,(xn,yn))
 *
 * For boxes, the points are opposite corners with the first point at the top right.
 * For closed paths and polygons, the points should be reordered to allow
 *  fast and correct equality comparisons.
 *
 * XXX perhaps points in complex shapes should be reordered internally
 *  to allow faster internal operations, but should keep track of input order
 *  and restore that order for text output - tgl 97/01/16
 */

int pair_decode(char *str, float8 *x, float8 *y, char **s);
int pair_encode(float8 x, float8 y, char *str);
int pair_count(char *s, char delim);
int path_decode(int opentype, int npts, char *str, int *isopen, char **ss, Point *p);

char *path_encode( bool closed, int npts, Point *pt);

int pair_decode(char *str, float8 *x, float8 *y, char **s)
{
    int has_delim;
    char *cp;

    if (!PointerIsValid((char *)str))
	return(FALSE);

    while (isspace( *str)) str++;
    if ((has_delim = (*str == LDELIM))) str++;

    while (isspace( *str)) str++;
    *x = strtod( str, &cp);
    if (cp <= str) return(FALSE);
    while (isspace( *cp)) cp++;
    if (*cp++ != DELIM) return(FALSE);
    while (isspace( *cp)) cp++;
    *y = strtod( cp, &str);
    if (str <= cp) return(FALSE);
    while (isspace( *str)) str++;
    if (has_delim) {
	if (*str != RDELIM) return(FALSE);
	str++;
	while (isspace( *str)) str++;
    };
    if (s != NULL) *s = str;

    return(TRUE);
}

int pair_encode(float8 x, float8 y, char *str)
{
    (void) sprintf(str, "%.*g,%.*g", digits8, x, digits8, y);
    return(TRUE);
}

int path_decode(int opentype, int npts, char *str, int *isopen, char **ss, Point *p)
{
    int depth = 0;
    char *s, *cp;
    int i;

    s = str;
    while (isspace( *s)) s++;
    if ((*isopen = (*s == LDELIM_EP))) {
	/* no open delimiter allowed? */
	if (! opentype) return(FALSE);
	depth++;
	s++;
	while (isspace( *s)) s++;

    } else if (*s == LDELIM) {
	cp = (s+1);
	while (isspace( *cp)) cp++;
	if (*cp == LDELIM) {
	    /* nested delimiters with only one point? */
	    if (npts <= 1) return(FALSE);
	    depth++;
	    s = cp;
	} else if (strrchr( s, LDELIM) == s) {
	    depth++;
	    s = cp;
	};
    };

    for (i = 0; i < npts; i++) {
	if (! pair_decode( s, &(p->x), &(p->y), &s))
	    return(FALSE);

	if (*s == DELIM) s++;
	p++;
    };

    while (depth > 0) {
	if ((*s == RDELIM)
         || ((*s == RDELIM_EP) && (*isopen) && (depth == 1))) {
	    depth--;
	    s++;
	    while (isspace( *s)) s++;
	} else {
	    return(FALSE);
	};
    };
    *ss = s;

    return(TRUE);
} /* path_decode() */

char *path_encode( bool closed, int npts, Point *pt)
{
    char *result;

    char *cp;
    int i;

    if (!PointerIsValid(result = (char *)PALLOC(npts*(P_MAXLEN+3)+2)))
	elog(WARN, "Memory allocation failed, can't output path", NULL);

    cp = result;
    switch (closed) {
    case TRUE:
	*cp++ = LDELIM;
	break;
    case FALSE:
	*cp++ = LDELIM_EP;
	break;
    default:
	break;
    };

    for (i = 0; i < npts; i++) {
        *cp++ = LDELIM;
	if (! pair_encode( pt->x, pt->y, cp))
	  elog (WARN, "Unable to format path", NULL);
	cp += strlen(cp);
	*cp++ = RDELIM;
	*cp++ = DELIM;
	pt++;
    };
    cp--;
    switch (closed) {
    case TRUE:
	*cp++ = RDELIM;
	break;
    case FALSE:
	*cp++ = RDELIM_EP;
	break;
    default:
	break;
    };
    *cp = '\0';

    return(result);
} /* path_encode() */

/*-------------------------------------------------------------
 * pair_count - count the number of points
 * allow the following notation:
 * '((1,2),(3,4))'
 * '(1,3,2,4)'
 * require an odd number of delim characters in the string
 *-------------------------------------------------------------*/
int pair_count(char *s, char delim)
{
    int ndelim = 0;

    while ((s = strchr( s, delim)) != NULL) {
	ndelim++;
	s++;
    };
    return((ndelim % 2)? ((ndelim+1)/2): -1);
}

/***********************************************************************
 **
 ** 	Routines for two-dimensional boxes.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * Formatting and conversion routines.
 *---------------------------------------------------------*/

/*	box_in	-	convert a string to internal form.
 *
 *	External format: (two corners of box)
 *		"(f8, f8), (f8, f8)"
 *		also supports the older style "(f8, f8, f8, f8)"
 */
BOX *box_in(char *str)
{
    BOX	*box;

    int isopen;
    char *s;
    double x, y;

    if (!PointerIsValid((char *)str))
	elog (WARN," Bad (null) box external representation",NULL);

    if (!PointerIsValid(box = PALLOCTYPE(BOX)))
      elog(WARN, "Memory allocation failed, can't input box '%s'",str);

    if ((! path_decode(FALSE, 2, str, &isopen, &s, &(box->high)))
      || (*s != '\0'))
	elog (WARN, "Bad box external representation '%s'",str);

    /* reorder corners if necessary... */
    if (box->high.x < box->low.x) {
	x = box->high.x;
	box->high.x = box->low.x;
	box->low.x = x;
    };
    if (box->high.y < box->low.y) {
	y = box->high.y;
	box->high.y = box->low.y;
	box->low.y = y;
    };

    return(box);
}

/*	box_out	-	convert a box to external form.
 */
char *box_out(BOX *box)
{
#if OLD_FORMAT_OUT
    char *result;

    char *cp;
#endif

    if (!PointerIsValid((char *)box))
	return(NULL);

#if OLD_FORMAT_OUT
    if (!PointerIsValid(result = (char *)PALLOC(2*(P_MAXLEN+1)+2)))
	elog(WARN, "Memory allocation failed, can't output box", NULL);

    cp = result;
    *cp++ = LDELIM;
    if (! pair_encode( box->high.x, box->high.y, cp))
	  elog (WARN, "Unable to format box", NULL);
    cp += strlen(cp);
    *cp++ = DELIM;
    if (! pair_encode( box->low.x, box->low.y, cp))
	  elog (WARN, "Unable to format box", NULL);
    cp += strlen(cp);
    *cp++ = RDELIM;
    *cp = '\0';

    return( result);
#else
    return( path_encode( -1, 2, (Point *) &(box->high)));
#endif
}


/*	box_construct	-	fill in a new box.
 */
BOX *box_construct(double x1, double x2, double y1, double y2)
{
    BOX	*result;
    
    result = PALLOCTYPE(BOX);
    return( box_fill(result, x1, x2, y1, y2) );
}


/*	box_fill	-	fill in a static box
 */
BOX *box_fill(BOX *result, double x1, double x2, double y1, double y2)
{
    if (x1 > x2) {
	result->high.x = x1;
	result->low.x = x2;
    } else {
	result->high.x = x2;
	result->low.x = x1;
    };
    if (y1 > y2) {
	result->high.y = y1;
	result->low.y = y2;
    } else {
	result->high.y = y2;
	result->low.y = y1;
    };
    
    return(result);
}


/*	box_copy	-	copy a box
 */
BOX *box_copy(BOX *box)
{
    BOX	*result;
    
    result = PALLOCTYPE(BOX);
    memmove((char *) result, (char *) box, sizeof(BOX));
    
    return(result);
}


/*----------------------------------------------------------
 *  Relational operators for BOXes.
 *	<, >, <=, >=, and == are based on box area.
 *---------------------------------------------------------*/

/*	box_same	-	are two boxes identical?
 */
bool box_same(BOX *box1, BOX *box2)
{
    return((FPeq(box1->high.x,box2->high.x) && FPeq(box1->low.x,box2->low.x)) &&
	  (FPeq(box1->high.y,box2->high.y) && FPeq(box1->low.y,box2->low.y)));
}

/*	box_overlap	-	does box1 overlap box2?
 */
bool box_overlap(BOX *box1, BOX *box2)
{
    return(((FPge(box1->high.x,box2->high.x) && FPle(box1->low.x,box2->high.x)) ||
	    (FPge(box2->high.x,box1->high.x) && FPle(box2->low.x,box1->high.x))) &&
	   ((FPge(box1->high.y,box2->high.y) && FPle(box1->low.y,box2->high.y)) ||
	    (FPge(box2->high.y,box1->high.y) && FPle(box2->low.y,box1->high.y))) );
}

/*	box_overleft	-	is the right edge of box1 to the left of
 *				the right edge of box2?
 *
 *	This is "less than or equal" for the end of a time range,
 *	when time ranges are stored as rectangles.
 */
bool box_overleft(BOX *box1, BOX *box2)
{
    return(FPle(box1->high.x,box2->high.x));
}

/*	box_left	-	is box1 strictly left of box2?
 */
bool box_left(BOX *box1, BOX *box2)
{
    return(FPlt(box1->high.x,box2->low.x));
}

/*	box_right	-	is box1 strictly right of box2?
 */
bool box_right(BOX *box1, BOX *box2)
{
    return(FPgt(box1->low.x,box2->high.x));
}

/*	box_overright	-	is the left edge of box1 to the right of
 *				the left edge of box2?
 *
 *	This is "greater than or equal" for time ranges, when time ranges
 *	are stored as rectangles.
 */
bool box_overright(BOX *box1, BOX *box2)
{
    return(box1->low.x >= box2->low.x);
}

/*	box_contained	-	is box1 contained by box2?
 */
bool box_contained(BOX *box1, BOX *box2)
{
    return((FPle(box1->high.x,box2->high.x) && FPge(box1->low.x,box2->low.x)) &&
	   (FPle(box1->high.y,box2->high.y) && FPge(box1->low.y,box2->low.y)));
}

/*	box_contain	-	does box1 contain box2?
 */
bool box_contain(BOX *box1, BOX *box2)
{
    return((FPge(box1->high.x,box2->high.x) && FPle(box1->low.x,box2->low.x) &&
	    FPge(box1->high.y,box2->high.y) && FPle(box1->low.y,box2->low.y)));
}


/*	box_positionop	-
 *		is box1 entirely {above,below} box2?
 */
bool box_below(BOX *box1, BOX *box2)
{
    return( FPle(box1->high.y,box2->low.y) );
}

bool box_above(BOX *box1, BOX *box2)
{
    return( FPge(box1->low.y,box2->high.y) );
}


/*	box_relop	-	is area(box1) relop area(box2), within
 *			  	our accuracy constraint?
 */
bool box_lt(BOX *box1, BOX *box2)
{
    return( FPlt(box_ar(box1), box_ar(box2)) );
}

bool box_gt(BOX *box1, BOX *box2)
{
    return( FPgt(box_ar(box1), box_ar(box2)) );
}

bool box_eq(BOX *box1, BOX *box2)
{
    return( FPeq(box_ar(box1), box_ar(box2)) );
}

bool box_le(BOX	*box1, BOX *box2)
{
    return( FPle(box_ar(box1), box_ar(box2)) );
}

bool box_ge(BOX	*box1, BOX *box2)
{
    return( FPge(box_ar(box1), box_ar(box2)) );
}


/*----------------------------------------------------------
 *  "Arithmetic" operators on boxes.
 *	box_foo	returns foo as an object (pointer) that
 can be passed between languages.
 *	box_xx	is an internal routine which returns the
 *		actual value (and cannot be handed back to
 *		LISP).
 *---------------------------------------------------------*/

/*	box_area	-	returns the area of the box.
 */
double *box_area(BOX *box)
{
    double *result;
    
    result = PALLOCTYPE(double);
    *result = box_ln(box) * box_ht(box);
    
    return(result);
}


/*	box_length	-	returns the length of the box 
 *				  (horizontal magnitude).
 */
double *box_length(BOX *box)
{
    double	*result;
    
    result = PALLOCTYPE(double);
    *result = box->high.x - box->low.x;
    
    return(result);
}


/*	box_height	-	returns the height of the box 
 *				  (vertical magnitude).
 */
double *box_height(BOX *box)
{
    double	*result;
    
    result = PALLOCTYPE(double);
    *result = box->high.y - box->low.y;
    
    return(result);
}


/*	box_distance	-	returns the distance between the
 *				  center points of two boxes.
 */
double *box_distance(BOX *box1, BOX *box2)
{
    double	*result;
    Point	*a, *b;
    
    result = PALLOCTYPE(double);
    a = box_center(box1);
    b = box_center(box2);
    *result = HYPOT(a->x - b->x, a->y - b->y);
    
    PFREE(a);
    PFREE(b);
    return(result);
}


/*	box_center	-	returns the center point of the box.
 */
Point *box_center(BOX *box)
{
    Point	*result;
    
    result = PALLOCTYPE(Point);
    result->x = (box->high.x + box->low.x) / 2.0;
    result->y = (box->high.y + box->low.y) / 2.0;
    
    return(result);
}


/*	box_ar	-	returns the area of the box.
 */
double box_ar(BOX *box)
{
    return( box_ln(box) * box_ht(box) );
}


/*	box_ln	-	returns the length of the box 
 *				  (horizontal magnitude).
 */
double box_ln(BOX *box)
{
    return( box->high.x - box->low.x );
}


/*	box_ht	-	returns the height of the box 
 *				  (vertical magnitude).
 */
double box_ht(BOX *box)
{
    return( box->high.y - box->low.y );
}


/*	box_dt	-	returns the distance between the
 *			  center points of two boxes.
 */
double box_dt(BOX *box1, BOX *box2)
{
    double	result;
    Point	*a, *b;
    
    a = box_center(box1);
    b = box_center(box2);
    result = HYPOT(a->x - b->x, a->y - b->y);
    
    PFREE(a);
    PFREE(b);
    return(result);
}

/*----------------------------------------------------------
 *  Funky operations.
 *---------------------------------------------------------*/

/*	box_intersect	-
 *		returns the overlapping portion of two boxes,
 *		  or NULL if they do not intersect.
 */
BOX *box_intersect(BOX	*box1, BOX *box2)
{
    BOX	*result;

    if (! box_overlap(box1,box2))
	return(NULL);
    result = PALLOCTYPE(BOX);
    result->high.x = Min(box1->high.x, box2->high.x);
    result->low.x = Max(box1->low.x, box2->low.x);
    result->high.y = Min(box1->high.y, box2->high.y);
    result->low.y = Max(box1->low.y, box2->low.y);
    
    return(result);
}


/*	box_diagonal	-	
 *		returns a line segment which happens to be the
 *		  positive-slope diagonal of "box".
 *		provided, of course, we have LSEGs.
 */
LSEG *box_diagonal(BOX *box)
{
    Point	p1, p2;
    
    p1.x = box->high.x;
    p1.y = box->high.y;
    p2.x = box->low.x;
    p2.y = box->low.y;
    return( lseg_construct( &p1, &p2 ) );
    
}

/***********************************************************************
 **
 ** 	Routines for 2D lines.
 **		Lines are not intended to be used as ADTs per se,
 **		but their ops are useful tools for other ADT ops.  Thus,
 **		there are few relops.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 *  Conversion routines from one line formula to internal.
 *	Internal form:	Ax+By+C=0
 *---------------------------------------------------------*/

LINE *				/* point-slope */
line_construct_pm(Point *pt, double m)
{
    LINE	*result;
    
    result = PALLOCTYPE(LINE);
    /* use "mx - y + yinter = 0" */
    result->A = m;
    result->B = -1.0;
    result->C = pt->y - m * pt->x;
    return(result);
}


LINE *				/* two points */
line_construct_pp(Point *pt1, Point *pt2)
{
    LINE	*result;
    
    result = PALLOCTYPE(LINE);
    if (FPeq(pt1->x, pt2->x)) {		/* vertical */
	/* use "x = C" */
	result->m = 0.0;
	result->A = -1.0;
	result->B = 0.0;
	result->C = pt1->x;
    } else {
	/* use "mx - y + yinter = 0" */
	result->m = (pt1->y - pt2->y) / (pt1->x - pt2->x);
	result->A = result->m;
	result->B = -1.0;
	result->C = pt1->y - result->m * pt1->x;
    }
    return(result);
}


/*----------------------------------------------------------
 *  Relative position routines.
 *---------------------------------------------------------*/

bool line_intersect(LINE *l1, LINE *l2)
{
    return( ! line_parallel(l1, l2) );
}

bool line_parallel(LINE *l1, LINE *l2)
{
    return( FPeq(l1->m, l2->m) );
}

bool line_perp(LINE *l1, LINE *l2)
{
    if (l1->m)
	return( FPeq(l2->m / l1->m, -1.0) );
    else if (l2->m)
	return( FPeq(l1->m / l2->m, -1.0) );
    return(1);	/* both 0.0 */
}

bool line_vertical(LINE *line)
{
    return( FPeq(line->A, -1.0) && FPzero(line->B) );
}

bool line_horizontal(LINE *line)
{
    return( FPzero(line->m) );
}


bool line_eq(LINE *l1, LINE *l2)
{
    double	k;
    
    if (! FPzero(l2->A))
	k = l1->A / l2->A;
    else if (! FPzero(l2->B))
	k = l1->B / l2->B;
    else if (! FPzero(l2->C))
	k = l1->C / l2->C;
    else
	k = 1.0;
    return( FPeq(l1->A, k * l2->A) &&
	   FPeq(l1->B, k * l2->B) &&
	   FPeq(l1->C, k * l2->C) );
}


/*----------------------------------------------------------
 *  Line arithmetic routines.
 *---------------------------------------------------------*/

double * 		/* distance between l1, l2 */
line_distance(LINE *l1, LINE *l2)
{
    double	*result;
    Point	*tmp;
    
    result = PALLOCTYPE(double);
    if (line_intersect(l1, l2)) {
	*result = 0.0;
	return(result);
    }
    if (line_vertical(l1))
	*result = fabs(l1->C - l2->C);
    else {
	tmp = point_construct(0.0, l1->C);
	result = dist_pl(tmp, l2);
	PFREE(tmp);
    }
    return(result);
}

Point *			/* point where l1, l2 intersect (if any) */
line_interpt(LINE *l1, LINE *l2)
{
    Point	*result;
    double	x;
    
    if (line_parallel(l1, l2))
	return(NULL);
    if (line_vertical(l1))
	result = point_construct(l2->m * l1->C + l2->C, l1->C);
    else if (line_vertical(l2))
	result = point_construct(l1->m * l2->C + l1->C, l2->C);
    else {
	x = (l1->C - l2->C) / (l2->A - l1->A);
	result = point_construct(x, l1->m * x + l1->C);
    }
    return(result);
}

/***********************************************************************
 **
 ** 	Routines for 2D paths (sequences of line segments, also
 **		called `polylines').
 **
 **		This is not a general package for geometric paths, 
 **		which of course include polygons; the emphasis here
 **		is on (for example) usefulness in wire layout.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 *  String to path / path to string conversion.
 *	External format: 
 *		"((xcoord, ycoord),... )"
 *		"[(xcoord, ycoord),... ]"
 *		"(xcoord, ycoord),... "
 *		"[xcoord, ycoord,... ]"
 *	Also support older format:
 *		"(closed, npts, xcoord, ycoord,... )"
 *---------------------------------------------------------*/

PATH *path_in(char *str)
{
    PATH *path;

    int isopen;
    char *s;
    int npts;
    int size;
#if OLD_FORMAT_IN
    int oldstyle = FALSE;
    double x, y;
#endif

    if (!PointerIsValid((char *)str))
	elog(WARN, "Bad (null) path external representation");

    if ((npts = pair_count(str, ',')) <= 0)
	elog(WARN, "Bad path external representation '%s'", str);

#if OLD_FORMAT_IN
    s = str;
    while (isspace( *s)) s++;
    /* identify old style format as having only one left delimiter in string... */
    oldstyle = ((*s == LDELIM) && (strrchr( s, LDELIM) == s));

    /* old-style format? then first two fields are closed flag and point count... */
    if (oldstyle) {
	s++;
	if ((! pair_decode( s, &x, &y, &s)) || (*s++ != DELIM)
	  || ((x != 0) && (x != 1)) || (y <= 0))
	    elog (WARN, "Bad path external representation '%s'",str);
	isopen = (x == 0);
	npts = y;
    };
#endif

    size = offsetof(PATH, p[0]) + (sizeof(path->p[0]) * npts);
    if (!PointerIsValid(path = PALLOC(size)))
	elog(WARN, "Memory allocation failed, can't input path '%s'",str);

    path->size = size;
    path->npts =  npts;
    if (oldstyle) path->closed = (! isopen);

#if OLD_FORMAT_IN
    if ((! path_decode(TRUE, npts, s, &isopen, &s, &(path->p[0])))
     || ! (oldstyle? (*s++ == RDELIM): (*s == '\0')))
#else
    if ((! path_decode(TRUE, npts, s, &isopen, &s, &(path->p[0])))
     || (*s != '\0'))
#endif
	elog (WARN, "Bad path external representation '%s'",str);

#if OLD_FORMAT_IN
    if (oldstyle) {
	while (isspace( *s)) s++;
	if (*s != '\0')
	    elog (WARN, "Bad path external representation '%s'",str);
    };
#endif

    if (! oldstyle) path->closed = (! isopen);

    return(path);
}


char *path_out(PATH *path)
{
#if OLD_FORMAT_OUT
    int i;
    char *result, *cp;
#endif

    if (!PointerIsValid((char *)path))
	return NULL;

#if OLD_FORMAT_OUT
    if (!PointerIsValid(result = (char *)PALLOC(path->npts*(P_MAXLEN+3)+2)))
	elog(WARN, "Memory allocation failed, can't output path", NULL);

    cp = result;
    *cp++ = LDELIM;
    if (! pair_encode( path->closed, path->npts, cp))
	elog (WARN, "Unable to format path", NULL);
    cp += strlen(cp);

    for (i=0; i<path->npts; i++) {
        *cp++ = DELIM;
	if (! pair_encode( path->p[i].x, path->p[i].y, cp))
	    elog (WARN, "Unable to format path", NULL);
	cp += strlen(cp);
    };
    *cp++ = RDELIM;
    *cp = '\0';
    return(result);
#else
    return( path_encode( path->closed, path->npts, (Point *) &(path->p[0])));
#endif
}


/*----------------------------------------------------------
 *  Relational operators.
 *	These are based on the path cardinality, 
 *	as stupid as that sounds.
 *
 *	Better relops and access methods coming soon.
 *---------------------------------------------------------*/

bool path_n_lt(PATH *p1, PATH *p2)
{
    return( (p1->npts < p2->npts ) );
}

bool path_n_gt(PATH *p1, PATH *p2)
{
    return( (p1->npts > p2->npts ) );
}

bool path_n_eq(PATH *p1, PATH *p2)
{
    return( (p1->npts == p2->npts) );
}

bool path_n_le(PATH *p1, PATH *p2)
{
    return( (p1->npts <= p2->npts ) );
}

bool path_n_ge(PATH *p1, PATH *p2)
{
    return( (p1->npts >= p2->npts ) );
}


/*----------------------------------------------------------
 * Conversion operators.
 *---------------------------------------------------------*/

PATH *path_copy(PATH *path);

bool
path_isclosed( PATH *path)
{
    if (!PointerIsValid((char *)path))
	return FALSE;

    return(path->closed);
} /* path_isclosed() */

bool
path_isopen( PATH *path)
{
    if (!PointerIsValid((char *)path))
	return FALSE;

    return(! path->closed);
} /* path_isopen() */


int4
path_npoints( PATH *path)
{
    if (!PointerIsValid((char *)path))
	return 0;

    return(path->npts);
} /* path_npoints() */

PATH *
path_close(PATH *path)
{
    PATH *result;

    if (PointerIsValid((char *)result = path_copy(path)))
	result->closed = TRUE;

    return(result);
} /* path_close() */

PATH *
path_open(PATH *path)
{
    PATH *result;

    if (PointerIsValid((char *)result = path_copy(path)))
	result->closed = FALSE;

    return(result);
} /* path_open() */


PATH *
path_copy(PATH *path)
{
    PATH *result;
    int size;

    if (!PointerIsValid((char *)path))
	return NULL;

    size = offsetof(PATH, p[0]) + (sizeof(path->p[0]) * path->npts);
    if (!PointerIsValid(result = PALLOC(size)))
	elog(WARN, "Memory allocation failed, can't copy path",NULL);

    memmove((char *) result, (char *) path, size);
    return(result);
} /* path_copy() */


/* path_inter -
 *	Does p1 intersect p2 at any point?
 *	Use bounding boxes for a quick (O(n)) check, then do a 
 *	O(n^2) iterative edge check.
 */
bool path_inter(PATH *p1, PATH *p2)
{
    BOX	b1, b2;
    int	i, j;
    LSEG seg1, seg2;
    
    b1.high.x = b1.low.x = p1->p[0].x;
    b1.high.y = b1.low.y = p1->p[0].y;
    for (i = 1; i < p1->npts; i++) {
	b1.high.x = Max(p1->p[i].x, b1.high.x);
	b1.high.y = Max(p1->p[i].y, b1.high.y);
	b1.low.x = Min(p1->p[i].x, b1.low.x);
	b1.low.y = Min(p1->p[i].y, b1.low.y);
    }
    b2.high.x = b2.low.x = p2->p[0].x;
    b2.high.y = b2.low.y = p2->p[0].y;
    for (i = 1; i < p2->npts; i++) {
	b2.high.x = Max(p2->p[i].x, b2.high.x);
	b2.high.y = Max(p2->p[i].y, b2.high.y);
	b2.low.x = Min(p2->p[i].x, b2.low.x);
	b2.low.y = Min(p2->p[i].y, b2.low.y);
    }
    if (! box_overlap(&b1, &b2))
	return(0);
    
    /*  pairwise check lseg intersections */
    for (i = 0; i < p1->npts - 1; i++) {
	for (j = 0; j < p2->npts - 1; j++) {
	    statlseg_construct(&seg1, &p1->p[i], &p1->p[i+1]);
	    statlseg_construct(&seg2, &p2->p[j], &p2->p[j+1]);
	    if (lseg_intersect(&seg1, &seg2))
		return(1);
	}
    }
    
    /* if we dropped through, no two segs intersected */
    return(0);
}

/* this essentially does a cartesian product of the lsegs in the
   two paths, and finds the min distance between any two lsegs */
double *path_distance(PATH *p1, PATH *p2)
{
    double *min = NULL, *tmp;
    int i,j;
    LSEG seg1, seg2;

/*
    statlseg_construct(&seg1, &p1->p[0], &p1->p[1]);
    statlseg_construct(&seg2, &p2->p[0], &p2->p[1]);
    min = lseg_distance(&seg1, &seg2);
*/

    for (i = 0; i < p1->npts - 1; i++)
	for (j = 0; j < p2->npts - 1; j++)
	    {
		statlseg_construct(&seg1, &p1->p[i], &p1->p[i+1]);
		statlseg_construct(&seg2, &p2->p[j], &p2->p[j+1]);
		
		tmp = lseg_distance(&seg1, &seg2);
		if ((min == NULL) || (*min < *tmp)) {
		    if (min != NULL) PFREE(min);
		    min = tmp;
		} else {
		    PFREE(tmp);
		};
	    }

    return(min);
}


/*----------------------------------------------------------
 *  "Arithmetic" operations.
 *---------------------------------------------------------*/

double *path_length(PATH *path)
{
    double *result;
    int	ct, i;
    
    result = PALLOCTYPE(double);
    ct = path->npts - 1;
    for (i = 0; i < ct; i++)
	*result += point_dt(&path->p[i], &path->p[i+1]);
    
    return(result);
}



double path_ln(PATH *path)
{
    double result;
    int	ct, i;
    
    ct = path->npts - 1;
    for (result = i = 0; i < ct; i++)
	result += point_dt(&path->p[i], &path->p[i+1]);
    
    return(result);
}
/***********************************************************************
 **
 ** 	Routines for 2D points.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 *  String to point, point to string conversion.
 *	External format:
 *		"(x,y)"
 *		"x,y"
 *---------------------------------------------------------*/

Point *
point_in(char *str)
{
    Point *point;

    double x, y;
    char *s;
    
    if (str == NULL) {
	elog(WARN, "Bad (null) point external representation");
	return NULL;
    }

    if (! pair_decode( str, &x, &y, &s) || (strlen(s) > 0))
      elog (WARN, "Bad point external representation '%s'",str);

    if (!PointerIsValid(point = PALLOCTYPE(Point)))
      elog (WARN, "Unable to allocate point storage for '%s'",str);

    point->x = x;
    point->y = y;

    return(point);
} /* point_in() */

char *
point_out(Point *pt)
{
    if (!PointerIsValid((char *)pt))
	return(NULL);

    return( path_encode( -1, 1, pt));
} /* point_out() */


Point *point_construct(double x, double y)
{
    Point *result;
    
    result = PALLOCTYPE(Point);
    result->x = x;
    result->y = y;
    return(result);
}


Point *point_copy(Point *pt)
{
    Point *result;
    
    result = PALLOCTYPE(Point);
    result->x = pt->x;
    result->y = pt->y;
    return(result);
}


/*----------------------------------------------------------
 *  Relational operators for Points.
 *	Since we do have a sense of coordinates being
 *	"equal" to a given accuracy (point_vert, point_horiz), 
 *	the other ops must preserve that sense.  This means
 *	that results may, strictly speaking, be a lie (unless
 *	EPSILON = 0.0).
 *---------------------------------------------------------*/

bool point_left(Point *pt1, Point *pt2)
{
    return( FPlt(pt1->x, pt2->x) );
}

bool point_right(Point *pt1, Point *pt2)
{
    return( FPgt(pt1->x, pt2->x) );
}

bool point_above(Point *pt1, Point *pt2)
{
    return( FPgt(pt1->y, pt2->y) );
}

bool point_below(Point *pt1, Point *pt2)
{
    return( FPlt(pt1->y, pt2->y) );
}

bool point_vert(Point *pt1, Point *pt2)
{
    return( FPeq( pt1->x, pt2->x ) );
}

bool point_horiz(Point *pt1, Point *pt2)
{
    return( FPeq( pt1->y, pt2->y ) );
}

bool point_eq(Point *pt1, Point *pt2)
{
    return( point_horiz(pt1, pt2) && point_vert(pt1, pt2) );
}

/*----------------------------------------------------------
 *  "Arithmetic" operators on points.
 *---------------------------------------------------------*/

int32 pointdist(Point *p1, Point *p2)
{
    int32 result;
    
    result = point_dt(p1, p2);
    return(result);
}

double *point_distance(Point *pt1, Point *pt2)
{
    double *result;
    
    result = PALLOCTYPE(double);
    *result = HYPOT( pt1->x - pt2->x, pt1->y - pt2->y );
    return(result);
}


double point_dt(Point *pt1, Point *pt2)
{
    return( HYPOT( pt1->x - pt2->x, pt1->y - pt2->y ) );
}

double *point_slope(Point *pt1, Point *pt2)
{
    double *result;
    
    result = PALLOCTYPE(double);
    if (point_vert(pt1, pt2))
	*result = (double)DBL_MAX;
    else
	*result = (pt1->y - pt2->y) / (pt1->x - pt1->x);
    return(result);
}


double point_sl(Point *pt1, Point *pt2)
{
    return(	point_vert(pt1, pt2)
	   ? (double)DBL_MAX
	   : (pt1->y - pt2->y) / (pt1->x - pt2->x) );
}


/***********************************************************************
 **
 ** 	Routines for 2D line segments.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 *  String to lseg, lseg to string conversion.
 *	External forms:	"[(x1, y1), (x2, y2)]"
 *			"(x1, y1), (x2, y2)"
 *			"x1, y1, x2, y2"
 *	closed form ok	"((x1, y1), (x2, y2))"
 *	(old form)	"(x1, y1, x2, y2)"
 *---------------------------------------------------------*/

LSEG *lseg_in(char *str)
{
    LSEG *lseg;

    int isopen;
    char *s;

    if (!PointerIsValid((char *)str))
	elog (WARN," Bad (null) lseg external representation",NULL);

    if (!PointerIsValid(lseg = PALLOCTYPE(LSEG)))
      elog(WARN, "Memory allocation failed, can't input lseg '%s'",str);

    if ((! path_decode(TRUE, 2, str, &isopen, &s, &(lseg->p[0])))
      || (*s != '\0'))
	elog (WARN, "Bad lseg external representation '%s'",str);

    lseg->m = point_sl(&lseg->p[0], &lseg->p[1]);
    
    return(lseg);
}


char *lseg_out(LSEG *ls)
{
    if (!PointerIsValid((char *)ls))
	return(NULL);

    return( path_encode( FALSE, 2, (Point *) &(ls->p[0])));
}


/* lseg_construct -
 *	form a LSEG from two Points.
 */
LSEG *lseg_construct(Point *pt1, Point *pt2)
{
    LSEG	*result;
    
    result = PALLOCTYPE(LSEG);
    result->p[0].x = pt1->x;
    result->p[0].y = pt1->y;
    result->p[1].x = pt2->x;
    result->p[1].y = pt2->y;

    result->m = point_sl(pt1, pt2);
    
    return(result);
}

/* like lseg_construct, but assume space already allocated */
void statlseg_construct(LSEG *lseg, Point *pt1, Point *pt2)
{
    lseg->p[0].x = pt1->x;
    lseg->p[0].y = pt1->y;
    lseg->p[1].x = pt2->x;
    lseg->p[1].y = pt2->y;

    lseg->m = point_sl(pt1, pt2);
}

/*----------------------------------------------------------
 *  Relative position routines.
 *---------------------------------------------------------*/

/*
 **  find intersection of the two lines, and see if it falls on 
 **  both segments.
 */
bool lseg_intersect(LSEG *l1, LSEG *l2)
{
    LINE *ln;
    Point *interpt;
    bool retval;
    
    ln = line_construct_pp(&l2->p[0], &l2->p[1]);
    interpt = interpt_sl(l1, ln);
    
    if (interpt != NULL && on_ps(interpt, l2)) /* interpt on l1 and l2 */
	retval = TRUE;
    else retval = FALSE;
    if (interpt != NULL) PFREE(interpt);
    PFREE(ln);
    return(retval);
}

bool lseg_parallel(LSEG *l1, LSEG *l2)
{
    return( FPeq(l1->m, l2->m) );
}

bool lseg_perp(LSEG *l1, LSEG *l2)
{
    if (! FPzero(l1->m))
	return( FPeq(l2->m / l1->m, -1.0) );
    else if (! FPzero(l2->m))
	return( FPeq(l1->m / l2->m, -1.0) );
    return(0);	/* both 0.0 */
}

bool lseg_vertical(LSEG *lseg)
{
    return( FPeq(lseg->p[0].x, lseg->p[1].x) );
}

bool lseg_horizontal(LSEG *lseg)
{
    return( FPeq(lseg->p[0].y, lseg->p[1].y) );
}


bool lseg_eq(LSEG *l1, LSEG *l2)
{
    return( FPeq(l1->p[0].x, l2->p[0].x) &&
	   FPeq(l1->p[1].y, l2->p[1].y) &&
	   FPeq(l1->p[0].x, l2->p[0].x) &&
	   FPeq(l1->p[1].y, l2->p[1].y) );
}


/*----------------------------------------------------------
 *  Line arithmetic routines.
 *---------------------------------------------------------*/

/* lseg_distance -
 *	If two segments don't intersect, then the closest
 *	point will be from one of the endpoints to the other
 *	segment.
 */
double *lseg_distance(LSEG *l1, LSEG *l2)
{
    double *result;
    
    result = PALLOCTYPE(double);
    *result = lseg_dt( l1, l2);

    return(result);
}

/* distance between l1, l2 */
double lseg_dt(LSEG *l1, LSEG *l2)
{
    double	*d, result;
    
    if (lseg_intersect(l1, l2))
	return(0.0);

    d = dist_ps(&l1->p[0], l2);
    result = *d;
    PFREE(d);
    d = dist_ps(&l1->p[1], l2);
    result = Min(result, *d);
    PFREE(d);
    d = dist_ps(&l2->p[0], l1);
    result = Min(result, *d);
    PFREE(d);
    d = dist_ps(&l2->p[1], l1);
    result = Min(result, *d);
    PFREE(d);
    
    return(result);
}


/* lseg_interpt -
 *	Find the intersection point of two segments (if any).
 *	Find the intersection of the appropriate lines; if the 
 *	point is not on a given segment, there is no valid segment
 *	intersection point at all.
 */
Point *lseg_interpt(LSEG *l1, LSEG *l2)
{
    Point	*result;
    LINE	*tmp1, *tmp2;
    
    tmp1 = line_construct_pp(&l1->p[0], &l1->p[1]);
    tmp2 = line_construct_pp(&l2->p[0], &l2->p[1]);
    result = line_interpt(tmp1, tmp2);
    if (result)
	if (! on_ps(result, l1)) {
	    PFREE(result);
	    result = NULL;
	}
    
    PFREE(tmp1);
    PFREE(tmp2);
    return(result);
}

/***********************************************************************
 **
 ** 	Routines for position comparisons of differently-typed
 **		2D objects.
 **
 ***********************************************************************/

#define	ABOVE	1
#define	BELOW	0
#define	UNDEF	-1


/*---------------------------------------------------------------------
 *	dist_
 *		Minimum distance from one object to another.
 *-------------------------------------------------------------------*/

double *dist_pl(Point *pt, LINE *line)
{
    double	*result;
    
    result = PALLOCTYPE(double);
    *result = (line->A * pt->x + line->B * pt->y + line->C) /
	HYPOT(line->A, line->B);
    
    return(result);
}

double *dist_ps(Point *pt, LSEG *lseg)
{
    double m;                       /* slope of perp. */
    LINE *ln;
    double *result, *tmpdist;
    Point *ip;
    
    /* construct a line that's perpendicular.  See if the intersection of
       the two lines is on the line segment. */
    if (lseg->p[1].x == lseg->p[0].x)
	m = 0;
    else if (lseg->p[1].y == lseg->p[0].y) /* slope is infinite */
	m = (double)DBL_MAX;
    else m = (-1) * (lseg->p[1].y - lseg->p[0].y) / 
	(lseg->p[1].x - lseg->p[0].x);
    ln = line_construct_pm(pt, m);
    
    if ((ip = interpt_sl(lseg, ln)) != NULL)
	result = point_distance(pt, ip);
    else  /* intersection is not on line segment, so distance is min
	     of distance from point to an endpoint */
	{
	    result = point_distance(pt, &lseg->p[0]);
	    tmpdist = point_distance(pt, &lseg->p[1]);
	    if (*tmpdist < *result) *result = *tmpdist;
	    PFREE (tmpdist);
	}
    
    if (ip != NULL) PFREE(ip);
    PFREE(ln);
    return (result);
}


/*
 ** Distance from a point to a path 
 */
double *dist_ppth(Point *pt, PATH *path)
{
    double *result;
    double *tmp;
    int i;
    LSEG lseg;
    
    switch (path->npts) {
    /* no points in path? then result is undefined... */
    case 0:
	result = NULL;
	break;
    /* one point in path? then get distance between two points... */
    case 1:
	result = point_distance(pt, &path->p[0]);
	break;
    default:
	/* make sure the path makes sense... */
	Assert(path->npts > 1);
	/*
	 * the distance from a point to a path is the smallest distance
	 * from the point to any of its constituent segments.
	 */
	result = PALLOCTYPE(double);
	for (i = 0; i < path->npts - 1; i++) {
	    statlseg_construct(&lseg, &path->p[i], &path->p[i+1]);
	    tmp = dist_ps(pt, &lseg);
	    if (i == 0 || *tmp < *result)
		*result = *tmp;
	    PFREE(tmp);
	}
	break;
    }
    return(result);
}

double *dist_pb(Point *pt, BOX *box)
{
    Point	*tmp;
    double	*result;
    
    tmp = close_pb(pt, box);
    result = point_distance(tmp, pt);
    PFREE(tmp);

    return(result);
}


double *dist_sl(LSEG *lseg, LINE *line)
{
    double *result, *d2;

    if (inter_sl(lseg, line)) {
	result = PALLOCTYPE(double);
	*result = 0.0;

    } else {
	result = dist_pl(&lseg->p[0], line);
	d2 = dist_pl(&lseg->p[1], line);
	if (*d2 > *result) {
	    PFREE( result);
	    result = d2;
	} else {
	    PFREE( d2);
	};
    };
    
    return(result);
}


double *dist_sb(LSEG *lseg, BOX *box)
{
    Point	*tmp;
    double	*result;
    
    tmp = close_sb(lseg, box);
    if (tmp == NULL) {
	result = PALLOCTYPE(double);
	*result = 0.0;
    } else {
	result = dist_pb(tmp, box);
	PFREE(tmp);
    }
    
    return(result);
}


double *dist_lb(LINE *line, BOX *box)
{
    Point	*tmp;
    double	*result;
    
    tmp = close_lb(line, box);
    if (tmp == NULL) {
	result = PALLOCTYPE(double);
	*result = 0.0;
    } else {
	result = dist_pb(tmp, box);
	PFREE(tmp);
    }
    
    return(result);
}


/*---------------------------------------------------------------------
 *	interpt_
 *		Intersection point of objects.
 *		We choose to ignore the "point" of intersection between 
 *		  lines and boxes, since there are typically two.
 *-------------------------------------------------------------------*/

Point *interpt_sl(LSEG *lseg, LINE *line)
{
    LINE	*tmp;
    Point	*p;
    
    tmp = line_construct_pp(&lseg->p[0], &lseg->p[1]);
    p = line_interpt(tmp, line);
    if (p)
	if (! on_ps(p, lseg)) {
	    PFREE(p);
	    p = NULL;
	}
    
    PFREE(tmp);
    return(p);
}


/*---------------------------------------------------------------------
 *	close_
 *		Point of closest proximity between objects.
 *-------------------------------------------------------------------*/

/* close_pl - 
 *	The intersection point of a perpendicular of the line 
 *	through the point.
 */
Point *close_pl(Point *pt, LINE *line)
{
    Point	*result;
    LINE	*tmp;
    double	invm;
    
    result = PALLOCTYPE(Point);
    if (FPeq(line->A, -1.0) && FPzero(line->B)) {	/* vertical */
	result->x = line->C;
	result->y = pt->y;
	return(result);
    } else if (FPzero(line->m)) {			/* horizontal */
	result->x = pt->x;
	result->y = line->C;
	return(result);
    }
    /* drop a perpendicular and find the intersection point */
    invm = -1.0 / line->m;
    tmp = line_construct_pm(pt, invm);
    result = line_interpt(tmp, line);
    return(result);
}


/* close_ps - 
 *	Take the closest endpoint if the point is left, right, 
 *	above, or below the segment, otherwise find the intersection
 *	point of the segment and its perpendicular through the point.
 */
Point *close_ps(Point *pt, LSEG *lseg)
{
    Point	*result;
    LINE	*tmp;
    double	invm;
    int	xh, yh;
    
    result = NULL;
    xh = lseg->p[0].x < lseg->p[1].x;
    yh = lseg->p[0].y < lseg->p[1].y;
    if (pt->x < lseg->p[!xh].x)
	result = point_copy(&lseg->p[!xh]);
    else if (pt->x > lseg->p[xh].x)
	result = point_copy(&lseg->p[xh]);
    else if (pt->y < lseg->p[!yh].y)
	result = point_copy(&lseg->p[!yh]);
    else if (pt->y > lseg->p[yh].y)
	result = point_copy(&lseg->p[yh]);
    if (result)
	return(result);
    if (FPeq(lseg->p[0].x, lseg->p[1].x)) {	/* vertical */
	result->x = lseg->p[0].x;
	result->y = pt->y;
	return(result);
    } else if (FPzero(lseg->m)) {			/* horizontal */
	result->x = pt->x;
	result->y = lseg->p[0].y;
	return(result);
    }
    
    invm = -1.0 / lseg->m;
    tmp = line_construct_pm(pt, invm);
    result = interpt_sl(lseg, tmp);
    return(result);
}

Point *close_pb(Point *pt, BOX *box)
{
    /* think about this one for a while */
    
    return(NULL);
}

Point *close_sl(LSEG *lseg, LINE *line)
{
    Point	*result;
    double	*d1, *d2;

    result = interpt_sl(lseg, line);
    if (result)
	return(result);
    d1 = dist_pl(&lseg->p[0], line);
    d2 = dist_pl(&lseg->p[1], line);
    if (d1 < d2)
	result = point_copy(&lseg->p[0]);
    else
	result = point_copy(&lseg->p[1]);
    
    PFREE(d1);
    PFREE(d2);
    return(result);
}

Point *close_sb(LSEG *lseg, BOX *box)
{
    /* think about this one for a while */
    
    return(NULL);
}

Point *close_lb(LINE *line, BOX *box)
{
    /* think about this one for a while */
    
    return(NULL);
}

/*---------------------------------------------------------------------
 *	on_
 *		Whether one object lies completely within another.
 *-------------------------------------------------------------------*/

/* on_pl -
 *	Does the point satisfy the equation? 
 */
bool on_pl(Point *pt, LINE *line)
{
    return( FPzero(line->A * pt->x + line->B * pt->y + line->C) );
}


/* on_ps -
 *	Determine colinearity by detecting a triangle inequality.
 */
bool on_ps(Point *pt, LSEG *lseg)
{
    return( FPeq (point_dt(pt, &lseg->p[0]) + point_dt(pt, &lseg->p[1]),
            point_dt(&lseg->p[0], &lseg->p[1])) );
}

bool on_pb(Point *pt, BOX *box)
{
    return( pt->x <= box->high.x && pt->x >= box->low.x &&
	   pt->y <= box->high.y && pt->y >= box->low.y );
}

/* on_ppath - 
 *	Whether a point lies within (on) a polyline.
 *	If open, we have to (groan) check each segment.
 *	If closed, we use the old O(n) ray method for point-in-polygon.
 *		The ray is horizontal, from pt out to the right.
 *		Each segment that crosses the ray counts as an 
 *		intersection; note that an endpoint or edge may touch 
 *		but not cross.
 *		(we can do p-in-p in lg(n), but it takes preprocessing)
 */
#define NEXT(A)	((A+1) % path->npts)	/* cyclic "i+1" */

bool on_ppath(Point *pt, PATH *path)
{
    int	above, next,	/* is the seg above the ray? */
    inter,		/* # of times path crosses ray */
    hi,		/* index inc of higher seg (0,1) */
    i, n;
    double a, b, x, 
    yh, yl, xh, xl;
    
    if (! path->closed) {		/*-- OPEN --*/
	n = path->npts - 1;
	a = point_dt(pt, &path->p[0]);
	for (i = 0; i < n; i++) {
	    b = point_dt(pt, &path->p[i+1]);
	    if (FPeq(a+b,
		     point_dt(&path->p[i], &path->p[i+1])))
		return(1);
	    a = b;
	}
	return(0);
    }
    
    inter = 0;			/*-- CLOSED --*/
    above = FPgt(path->p[0].y, pt->y) ? ABOVE : 
	FPlt(path->p[0].y, pt->y) ? BELOW : UNDEF;
    
    for (i = 0; i < path->npts; i++) {
	hi = path->p[i].y < path->p[NEXT(i)].y;
	/* must take care of wrap around to original vertex for closed paths */
	yh = (i+hi < path->npts) ? path->p[i+hi].y : path->p[0].y;
	yl = (i+!hi < path->npts) ? path->p[i+!hi].y : path->p[0].y;
	hi = path->p[i].x < path->p[NEXT(i)].x;
	xh = (i+hi < path->npts) ? path->p[i+hi].x : path->p[0].x;
	xl = (i+!hi < path->npts) ? path->p[i+!hi].x : path->p[0].x;
	/* skip seg if it doesn't touch the ray */
	
	if (FPeq(yh, yl))	/* horizontal seg? */
	    if (FPge(pt->x, xl) && FPle(pt->x, xh) &&
		FPeq(pt->y, yh))
		return(1);	/* pt lies on seg */
	    else
		continue;	/* skip other hz segs */
	if (FPlt(yh, pt->y) ||	/* pt is strictly below seg */
	    FPgt(yl, pt->y))	/* strictly above */
	    continue;
	
	/* seg touches the ray, find out where */
	
	x = FPeq(xh, xl)	/* vertical seg? */
	    ? path->p[i].x	
		: (pt->y - path->p[i].y) / 
		    point_sl(&path->p[i],
			     &path->p[NEXT(i)]) +
				 path->p[i].x;
	if (FPeq(x, pt->x))	/* pt lies on this seg */
	    return(1);
	
	/* does the seg actually cross the ray? */
	
	next = FPgt(path->p[NEXT(i)].y, pt->y) ? ABOVE : 
	    FPlt(path->p[NEXT(i)].y, pt->y) ? BELOW : above;
	inter += FPge(x, pt->x) && next != above;
	above = next;
    }
    return(	above == UNDEF || 	/* path is horizontal */
	   inter % 2);		/* odd # of intersections */
}


bool on_sl(LSEG *lseg, LINE *line)
{
    return( on_pl(&lseg->p[0], line) && on_pl(&lseg->p[1], line) );
}

bool on_sb(LSEG *lseg, BOX *box)
{
    return( on_pb(&lseg->p[0], box) && on_pb(&lseg->p[1], box) );
}

/*---------------------------------------------------------------------
 *	inter_
 *		Whether one object intersects another.
 *-------------------------------------------------------------------*/

bool inter_sl(LSEG *lseg, LINE *line)
{
    Point	*tmp;

    tmp = interpt_sl(lseg, line);
    if (tmp) {
	PFREE(tmp);
	return(1);
    }
    return(0);
}

/* XXX segment and box should be able to intersect; tgl - 97/01/09 */

bool inter_sb(LSEG *lseg, BOX *box)
{
    return(0);
}

/* XXX line and box should be able to intersect; tgl - 97/01/09 */

bool inter_lb(LINE *line, BOX *box)
{
    return(0);
}

/*------------------------------------------------------------------
 * The following routines define a data type and operator class for
 * POLYGONS .... Part of which (the polygon's bounding box) is built on 
 * top of the BOX data type.
 *
 * make_bound_box - create the bounding box for the input polygon
 *------------------------------------------------------------------*/

/*---------------------------------------------------------------------
 * Make the smallest bounding box for the given polygon.
 *---------------------------------------------------------------------*/
void make_bound_box(POLYGON *poly)
{
    int i;
    double x1,y1,x2,y2;

    if (poly->npts > 0) {
	x2 = x1 = poly->p[0].x;
	y2 = y1 = poly->p[0].y;
	for (i = 1; i < poly->npts; i++) {
	    if (poly->p[i].x < x1) x1 = poly->p[i].x;
	    if (poly->p[i].x > x2) x2 = poly->p[i].x;
	    if (poly->p[i].y < y1) y1 = poly->p[i].y;
	    if (poly->p[i].y > y2) y2 = poly->p[i].y;
	};

	box_fill(&(poly->boundbox), x1, x2, y1, y2); 
    } else {
	elog (WARN, "Unable to create bounding box for empty polygon", NULL);
    };
}

/*------------------------------------------------------------------
 * poly_in - read in the polygon from a string specification
 *
 *	External format:
 *              "((x0,y0),...,(xn,yn))"
 *              "x0,y0,...,xn,yn"
 *		also supports the older style "(x1,...,xn,y1,...yn)"
 *------------------------------------------------------------------*/
POLYGON *poly_in(char *str)
{
    POLYGON *poly;
    int npts;
    int size;
    int isopen;

#if OLD_FORMAT_IN
    char *s;
    int oldstyle;
    int oddcount;
    int i;
    double x1, x2;
#endif

    if (!PointerIsValid((char *)str))
	elog (WARN," Bad (null) polygon external representation");

    if ((npts = pair_count(str, ',')) <= 0)
	elog(WARN, "Bad polygon external representation '%s'", str);

    size = offsetof(POLYGON, p[0]) + (sizeof(poly->p[0]) * npts);
    if (!PointerIsValid(poly = (POLYGON *) PALLOC(size)))
      elog(WARN, "Memory allocation failed, can't input polygon '%s'",str);

    memset((char *) poly, 0, size);	/* zero any holes */
    poly->size = size;
    poly->npts = npts;

#if OLD_FORMAT_IN
    s = str;
    while (isspace( *s)) s++;
    /* identify old style format as having only one left delimiter in string... */
    oldstyle = ((*s == LDELIM) && (strrchr( s, LDELIM) == s));

    if (oldstyle) {
	s++;
	while (isspace( *s)) s++;

	for (i=0; i<npts/2; i++) {
	    if (! pair_decode( s, &x1, &x2, &s))
		elog (WARN, "Bad polygon external representation '%s'",str);

	    if (*s == DELIM) s++;
	    poly->p[i*2].x = x1;
	    poly->p[i*2+1].x = x2;
	};
	oddcount = (npts % 2);
	if (oddcount) {
	    if (! pair_decode( s, &x1, &x2, &s))
		elog (WARN, "Bad polygon external representation '%s'",str);

	    if (*s == DELIM) s++;
	    poly->p[npts-1].x = x1;
	    poly->p[0].y = x2;
	};
	for (i=0; i<npts/2; i++) {
	    if (! pair_decode( s, &x1, &x2, &s))
		elog (WARN, "Bad polygon external representation '%s'",str);

	    if (*s == DELIM) s++;
	    poly->p[i*2+oddcount].y = x1;
	    poly->p[i*2+1+oddcount].y = x2;
	};

	if (*s == RDELIM) {
	    s++;
	    while (isspace( *s)) s++;
	    if (*s != '\0')
		elog(WARN, "Bad polygon external representation '%s'", str);

	} else {
	    elog(WARN, "Bad polygon external representation '%s'", str);
	};

    } else {
#endif
	if ((! path_decode(FALSE, npts, str, &isopen, &s, &(poly->p[0])))
	  || (*s != '\0'))
	elog (WARN, "Bad polygon external representation '%s'",str);

#if OLD_FORMAT_IN
    };
#endif;

    make_bound_box(poly);

    return( poly);
} /* poly_in() */

/*---------------------------------------------------------------
 * poly_out - convert internal POLYGON representation to the 
 *            character string format "((f8,f8),...,(f8,f8))"
 *            also support old format "(f8,f8,...,f8,f8)"
 *---------------------------------------------------------------*/
char *poly_out(POLYGON *poly)
{
#if OLD_FORMAT_OUT
    int i;
    char *result, *cp;
#endif

    if (!PointerIsValid((char *)poly))
	return NULL;

#if OLD_FORMAT_OUT
    if (!PointerIsValid(result = (char *)PALLOC(poly->npts*(P_MAXLEN+3)+2)))
	elog(WARN, "Memory allocation failed, can't output polygon", NULL);

    cp = result;
    *cp++ = LDELIM;

    for (i=0; i<poly->npts; i++) {
	if (! pair_encode( poly->p[i].x, poly->p[i].y, cp))
	    elog (WARN, "Unable to format polygon", NULL);
	cp += strlen(cp);
	*cp++ = DELIM;
    };
    *(cp-1) = RDELIM;
    *cp = '\0';
    return(result);
#else
    return( path_encode( TRUE, poly->npts, &(poly->p[0])));
#endif
}


/*-------------------------------------------------------
 * Is polygon A strictly left of polygon B? i.e. is
 * the right most point of A left of the left most point
 * of B?
 *-------------------------------------------------------*/
bool poly_left(POLYGON *polya, POLYGON *polyb)
{
    return (polya->boundbox.high.x < polyb->boundbox.low.x);
}

/*-------------------------------------------------------
 * Is polygon A overlapping or left of polygon B? i.e. is
 * the left most point of A left of the right most point
 * of B?
 *-------------------------------------------------------*/
bool poly_overleft(POLYGON *polya, POLYGON *polyb)
{
    return (polya->boundbox.low.x <= polyb->boundbox.high.x);
}

/*-------------------------------------------------------
 * Is polygon A strictly right of polygon B? i.e. is
 * the left most point of A right of the right most point
 * of B?
 *-------------------------------------------------------*/
bool poly_right(POLYGON *polya, POLYGON *polyb)
{
    return( polya->boundbox.low.x > polyb->boundbox.high.x);
}

/*-------------------------------------------------------
 * Is polygon A overlapping or right of polygon B? i.e. is
 * the right most point of A right of the left most point
 * of B?
 *-------------------------------------------------------*/
bool poly_overright(POLYGON *polya, POLYGON *polyb)
{
    return( polya->boundbox.high.x > polyb->boundbox.low.x);
}

/*-------------------------------------------------------
 * Is polygon A the same as polygon B? i.e. are all the
 * points the same?
 *-------------------------------------------------------*/
bool poly_same(POLYGON *polya, POLYGON *polyb)
{
    int i;
    if (polya->npts != polyb->npts)
	return FALSE;

    for (i = 0; i < polya->npts; i++) {
	if ((polya->p[i].x != polyb->p[i].x)
	 || (polya->p[i].y != polyb->p[i].y))
	    return FALSE;
    };
    return TRUE;
}

/*-----------------------------------------------------------------
 * Determine if polygon A overlaps polygon B by determining if
 * their bounding boxes overlap.
 *-----------------------------------------------------------------*/
bool poly_overlap(POLYGON *polya, POLYGON *polyb)
{
    return box_overlap(&(polya->boundbox), &(polyb->boundbox));
}

/*-----------------------------------------------------------------
 * Determine if polygon A contains polygon B by determining if A's
 * bounding box contains B's bounding box.
 *-----------------------------------------------------------------*/
bool poly_contain(POLYGON *polya, POLYGON *polyb)
{
    return box_contain(&(polya->boundbox), &(polyb->boundbox));
}

/*-----------------------------------------------------------------
 * Determine if polygon A is contained by polygon B by determining 
 * if A's bounding box is contained by B's bounding box.
 *-----------------------------------------------------------------*/
bool poly_contained(POLYGON *polya, POLYGON *polyb)
{
    return box_contained(&(polya->boundbox), &(polyb->boundbox));
}


/***********************************************************************
 **
 ** 	Routines for 2D points.
 **
 ***********************************************************************/

Point *
point(float8 *x, float8 *y)
{
    if (! (PointerIsValid(x) && PointerIsValid(y)))
	return(NULL);

    return(point_construct(*x, *y));
} /* point() */


Point *
point_add(Point *p1, Point *p2)
{
    Point *result;

    if (! (PointerIsValid(p1) && PointerIsValid(p2)))
	return(NULL);

    if (!PointerIsValid(result = PALLOCTYPE(Point)))
	elog(WARN, "Memory allocation failed, can't add points",NULL);

    result->x = (p1->x + p2->x);
    result->y = (p1->y + p2->y);

    return(result);
} /* point_add() */

Point *
point_sub(Point *p1, Point *p2)
{
    Point *result;

    if (! (PointerIsValid(p1) && PointerIsValid(p2)))
	return(NULL);

    if (!PointerIsValid(result = PALLOCTYPE(Point)))
	elog(WARN, "Memory allocation failed, can't add points",NULL);

    result->x = (p1->x - p2->x);
    result->y = (p1->y - p2->y);

    return(result);
} /* point_sub() */

Point *
point_mul(Point *p1, Point *p2)
{
    Point *result;

    if (! (PointerIsValid(p1) && PointerIsValid(p2)))
	return(NULL);

    if (!PointerIsValid(result = PALLOCTYPE(Point)))
	elog(WARN, "Memory allocation failed, can't multiply points",NULL);

    result->x = (p1->x*p2->x) - (p1->y*p2->y);
    result->y = (p1->x*p2->y) + (p1->y*p2->x);

    return(result);
} /* point_mul() */

Point *
point_div(Point *p1, Point *p2)
{
    Point *result;
    double div;

    if (! (PointerIsValid(p1) && PointerIsValid(p2)))
	return(NULL);

    if (!PointerIsValid(result = PALLOCTYPE(Point)))
	elog(WARN, "Memory allocation failed, can't multiply path",NULL);

    div = (p2->x*p2->x) + (p2->y*p2->y);

    result->x = ((p1->x*p2->x) + (p1->y*p2->y)) / div;
    result->y = ((p2->x*p1->y) - (p2->y*p1->x)) / div;

    return(result);
} /* point_div() */


/***********************************************************************
 **
 ** 	Routines for 2D boxes.
 **
 ***********************************************************************/

BOX *
box(Point *p1, Point *p2)
{
    BOX *result;

    if (! (PointerIsValid(p1) && PointerIsValid(p2)))
	return(NULL);

    result = box_construct( p1->x, p2->x, p1->y, p2->y);

    return(result);
} /* box() */

BOX *
box_add(BOX *box, Point *p)
{
    BOX *result;

    if (! (PointerIsValid(box) && PointerIsValid(p)))
	return(NULL);

    result = box_construct( (box->high.x + p->x), (box->low.x + p->x),
      (box->high.y + p->y), (box->low.y + p->y));

    return(result);
} /* box_add() */

BOX *
box_sub(BOX *box, Point *p)
{
    BOX *result;

    if (! (PointerIsValid(box) && PointerIsValid(p)))
	return(NULL);

    result = box_construct( (box->high.x - p->x), (box->low.x - p->x),
      (box->high.y - p->y), (box->low.y - p->y));

    return(result);
} /* box_sub() */

BOX *
box_mul(BOX *box, Point *p)
{
    BOX *result;
    Point *high, *low;

    if (! (PointerIsValid(box) && PointerIsValid(p)))
	return(NULL);

    high = point_mul( &box->high, p);
    low = point_mul( &box->low, p);

    result = box_construct( high->x, low->x, high->y, low->y);
    PFREE( high);
    PFREE( low);

    return(result);
} /* box_mul() */

BOX *
box_div(BOX *box, Point *p)
{
    BOX *result;
    Point *high, *low;

    if (! (PointerIsValid(box) && PointerIsValid(p)))
	return(NULL);

    high = point_div( &box->high, p);
    low = point_div( &box->low, p);

    result = box_construct( high->x, low->x, high->y, low->y);
    PFREE( high);
    PFREE( low);

    return(result);
} /* box_div() */


/***********************************************************************
 **
 ** 	Routines for 2D lines.
 **		Lines are not intended to be used as ADTs per se,
 **		but their ops are useful tools for other ADT ops.  Thus,
 **		there are few relops.
 **
 ***********************************************************************/


/***********************************************************************
 **
 ** 	Routines for 2D paths.
 **
 ***********************************************************************/

POLYGON *path_poly(PATH *path);

/* path_add()
 * Concatenate two paths (only if they are both open).
 */
PATH *
path_add(PATH *p1, PATH *p2)
{
    PATH *result;
    int size;
    int i;

    if (! (PointerIsValid(p1) && PointerIsValid(p2))
      || p1->closed || p2->closed)
	return(NULL);

    size = offsetof(PATH, p[0]) + (sizeof(p1->p[0]) * (p1->npts+p2->npts));
    if (!PointerIsValid(result = PALLOC(size)))
	elog(WARN, "Memory allocation failed, can't add paths",NULL);

    result->size = size;
    result->npts = (p1->npts+p2->npts);
    result->closed = p1->closed;

    for (i=0; i<p1->npts; i++) {
	result->p[i].x = p1->p[i].x;
	result->p[i].y = p1->p[i].y;
    };
    for (i=0; i<p2->npts; i++) {
	result->p[i+p1->npts].x = p2->p[i].x;
	result->p[i+p1->npts].y = p2->p[i].y;
    };

    return(result);
} /* path_add() */

/* path_add_pt()
 * Translation operator.
 */
PATH *
path_add_pt(PATH *path, Point *point)
{
    PATH *result;
    int i;

    if (! (PointerIsValid(path) && PointerIsValid(point)))
	return(NULL);

    if (! PointerIsValid(result = path_copy(path)))
	elog(WARN, "Memory allocation failed, can't add path",NULL);

    for (i=0; i<path->npts; i++) {
	result->p[i].x += point->x;
	result->p[i].y += point->y;
    };

    return(result);
} /* path_add_pt() */

PATH *
path_sub_pt(PATH *path, Point *point)
{
    PATH *result;
    int i;

    if (! (PointerIsValid(path) && PointerIsValid(point)))
	return(NULL);

    if (! PointerIsValid(result = path_copy(path)))
	elog(WARN, "Memory allocation failed, can't subtract path",NULL);

    for (i=0; i<path->npts; i++) {
	result->p[i].x -= point->x;
	result->p[i].y -= point->y;
    };

    return(result);
} /* path_sub_pt() */


/* path_mul_pt()
 * Rotation and scaling operators.
 */
PATH *
path_mul_pt(PATH *path, Point *point)
{
    PATH *result;
    Point *p;
    int i;

    if (! (PointerIsValid(path) && PointerIsValid(point)))
	return(NULL);

    if (! PointerIsValid(result = path_copy(path)))
	elog(WARN, "Memory allocation failed, can't multiply path",NULL);

    for (i=0; i<path->npts; i++) {
	p = point_mul( &path->p[i], point);
	result->p[i].x = p->x;
	result->p[i].y = p->y;
	PFREE(p);
    };

    return(result);
} /* path_mul_pt() */

PATH *
path_div_pt(PATH *path, Point *point)
{
    PATH *result;
    Point *p;
    int i;

    if (! (PointerIsValid(path) && PointerIsValid(point)))
	return(NULL);

    if (! PointerIsValid(result = path_copy(path)))
	elog(WARN, "Memory allocation failed, can't divide path",NULL);

    for (i=0; i<path->npts; i++) {
	p = point_div( &path->p[i], point);
	result->p[i].x = p->x;
	result->p[i].y = p->y;
	PFREE(p);
    };

    return(result);
} /* path_div_pt() */


POLYGON *path_poly(PATH *path)
{
    POLYGON *poly;
    int size;
    int i;

    if (!PointerIsValid(path))
	return(NULL);

    if (!path->closed)
	elog(WARN, "Open path cannot be converted to polygon",NULL);

    size = offsetof(POLYGON, p[0]) + (sizeof(poly->p[0]) * path->npts);
    if (!PointerIsValid(poly = PALLOC(size)))
	elog(WARN, "Memory allocation failed, can't convert path to polygon",NULL);

    poly->size = size;
    poly->npts = path->npts;

    for (i=0; i<path->npts; i++) {
	poly->p[i].x = path->p[i].x;
	poly->p[i].y = path->p[i].y;
    };

    make_bound_box(poly);

    return(poly);
} /* path_polygon() */


/***********************************************************************
 **
 ** 	Routines for 2D polygons.
 **
 ***********************************************************************/

int4
poly_npoints( POLYGON *poly)
{
    if (!PointerIsValid(poly))
	return(0);

    return(poly->npts);
} /* poly_npoints() */

BOX *
poly_box(POLYGON *poly)
{
    BOX *box;

    if (!PointerIsValid(poly) || (poly->npts < 1))
	return(NULL);

    box = box_copy( &poly->boundbox);

    return(box);
} /* poly_box() */

POLYGON *
box_poly(BOX *box)
{
    POLYGON *poly;
    int size;

    if (!PointerIsValid(box))
	return(NULL);

    size = offsetof(POLYGON, p[0]) + (sizeof(poly->p[0]) * 4);
    if (!PointerIsValid(poly = PALLOC(size)))
	elog(WARN, "Memory allocation failed, can't convert box to polygon",NULL);

    poly->size = size;
    poly->npts = 4;

    poly->p[0].x = box->low.x;
    poly->p[0].y = box->low.y;
    poly->p[1].x = box->low.x;
    poly->p[1].y = box->high.y;
    poly->p[2].x = box->high.x;
    poly->p[2].y = box->high.y;
    poly->p[3].x = box->high.x;
    poly->p[3].y = box->low.y;

    box_fill( &poly->boundbox, box->high.x, box->low.x, box->high.y, box->low.y);

    return(poly);
} /* box_poly() */

PATH *
poly_path(POLYGON *poly)
{
    PATH *path;
    int size;
    int i;

    if (!PointerIsValid(poly) || (poly->npts < 0))
	return(NULL);

    size = offsetof(PATH, p[0]) + (sizeof(path->p[0]) * poly->npts);
    if (!PointerIsValid(path = PALLOC(size)))
	elog(WARN, "Memory allocation failed, can't convert polygon to path",NULL);

    path->size = size;
    path->npts = poly->npts;
    path->closed = TRUE;

    for (i=0; i<poly->npts; i++) {
	path->p[i].x = poly->p[i].x;
	path->p[i].y = poly->p[i].y;
    };

    return(path);
} /* poly_path() */


/*-------------------------------------------------------------------------
 *
 * circle.c--
 *    2D geometric operations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/geo_ops.c,v 1.3 1997/04/22 17:31:32 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PI
#define PI 3.1415926536
#endif

int single_decode(char *str, float8 *x, char **ss);
int single_encode(float8 x, char *str);

int single_decode(char *str, float8 *x, char **s)
{
    char *cp;

    if (!PointerIsValid(str))
	return(FALSE);

    while (isspace( *str)) str++;
    *x = strtod( str, &cp);
#ifdef GEODEBUG
fprintf( stderr, "single_decode- (%x) try decoding %s to %g\n", (cp-str), str, *x);
#endif
    if (cp <= str) return(FALSE);
    while (isspace( *cp)) cp++;

    if (s != NULL) *s = cp;

    return(TRUE);
}

int single_encode(float8 x, char *str)
{
    (void) sprintf(str, "%.*g", digits8, x);
    return(TRUE);
}


/***********************************************************************
 **
 ** 	Routines for circles.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * Formatting and conversion routines.
 *---------------------------------------------------------*/

/*	circle_in	-	convert a string to internal form.
 *
 *	External format: (center and radius of circle)
 *		"((f8,f8)<f8>)"
 *		also supports quick entry style "(f8,f8,f8)"
 */
CIRCLE *circle_in(char *str)
{
    CIRCLE *circle;

    char *s, *cp;
    int depth = 0;

    if (!PointerIsValid(str))
	elog (WARN," Bad (null) circle external representation",NULL);

    if (!PointerIsValid(circle = PALLOCTYPE(CIRCLE)))
      elog(WARN, "Memory allocation failed, can't input circle '%s'",str);

    s = str;
    while (isspace( *s)) s++;
    if ((*s == LDELIM_C) || (*s == LDELIM)) {
	depth++;
	cp = (s+1);
	while (isspace( *cp)) cp++;
	if (*cp == LDELIM) {
	    s = cp;
	};
    };

    if (! pair_decode( s, &circle->center.x, &circle->center.y, &s))
      elog (WARN, "Bad circle external representation '%s'",str);

    if (*s == DELIM) s++;
    while (isspace( *s)) s++;

    if (! single_decode( s, &circle->radius, &s))
      elog (WARN, "Bad circle external representation '%s'",str);

    while (depth > 0) {
	if ((*s == RDELIM)
         || ((*s == RDELIM_C) && (depth == 1))) {
	    depth--;
	    s++;
	    while (isspace( *s)) s++;
	} else {
	    elog (WARN, "Bad circle external representation '%s'",str);
	};
    };

    if (*s != '\0')
      elog (WARN, "Bad circle external representation '%s'",str);

    return(circle);
} /* circle_in() */

/*	circle_out	-	convert a circle to external form.
 */
char *circle_out(CIRCLE *circle)
{
    char *result;
    char *cp;

    if (!PointerIsValid(circle))
	return(NULL);

    if (!PointerIsValid(result = (char *)PALLOC(3*(P_MAXLEN+1)+3)))
	elog(WARN, "Memory allocation failed, can't output circle", NULL);

    cp = result;
    *cp++ = LDELIM_C;
    *cp++ = LDELIM;
    if (! pair_encode( circle->center.x, circle->center.y, cp))
	  elog (WARN, "Unable to format circle", NULL);

    cp += strlen(cp);
    *cp++ = RDELIM;
    *cp++ = DELIM;
    if (! single_encode( circle->radius, cp))
	  elog (WARN, "Unable to format circle", NULL);

    cp += strlen(cp);
    *cp++ = RDELIM_C;
    *cp = '\0';

    return(result);
} /* circle_out() */


/*----------------------------------------------------------
 *  Relational operators for CIRCLEs.
 *	<, >, <=, >=, and == are based on circle area.
 *---------------------------------------------------------*/

/*	circles identical?
 */
bool circle_same(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPeq(circle1->radius,circle2->radius)
      && FPeq(circle1->center.x,circle2->center.x)
      && FPeq(circle1->center.y,circle2->center.y));
}

/*	circle_overlap	-	does circle1 overlap circle2?
 */
bool circle_overlap(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPle(point_dt(&circle1->center,&circle2->center),(circle1->radius+circle2->radius)));
}

/*	circle_overleft	-	is the right edge of circle1 to the left of
 *				the right edge of circle2?
 */
bool circle_overleft(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPle((circle1->center.x+circle1->radius),(circle2->center.x+circle2->radius)));
}

/*	circle_left	-	is circle1 strictly left of circle2?
 */
bool circle_left(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPle((circle1->center.x+circle1->radius),(circle2->center.x-circle2->radius)));
}

/*	circle_right	-	is circle1 strictly right of circle2?
 */
bool circle_right(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPge((circle1->center.x-circle1->radius),(circle2->center.x+circle2->radius)));
}

/*	circle_overright	-	is the left edge of circle1 to the right of
 *				the left edge of circle2?
 */
bool circle_overright(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPge((circle1->center.x-circle1->radius),(circle2->center.x-circle2->radius)));
}

/*	circle_contained	-	is circle1 contained by circle2?
 */
bool circle_contained(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPle((point_dt(&circle1->center,&circle2->center)+circle1->radius),circle2->radius));
}

/*	circle_contain	-	does circle1 contain circle2?
 */
bool circle_contain(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPle((point_dt(&circle1->center,&circle2->center)+circle2->radius),circle1->radius));
}


/*	circle_positionop	-
 *		is circle1 entirely {above,below} circle2?
 */
bool circle_below(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPle((circle1->center.y+circle1->radius),(circle2->center.y-circle2->radius)));
}

bool circle_above(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPge((circle1->center.y-circle1->radius),(circle2->center.y+circle2->radius)));
}


/*	circle_relop	-	is area(circle1) relop area(circle2), within
 *			  	our accuracy constraint?
 */
bool circle_eq(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPeq(circle_ar(circle1), circle_ar(circle2)) );
} /* circle_eq() */

bool circle_ne(CIRCLE *circle1, CIRCLE *circle2)
{
    return( !circle_eq(circle1, circle2));
} /* circle_ne() */

bool circle_lt(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPlt(circle_ar(circle1), circle_ar(circle2)) );
} /* circle_lt() */

bool circle_gt(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPgt(circle_ar(circle1), circle_ar(circle2)) );
} /* circle_gt() */

bool circle_le(CIRCLE *circle1, CIRCLE *circle2)
{
    return( FPle(circle_ar(circle1), circle_ar(circle2)) );
} /* circle_le() */

bool circle_ge(CIRCLE	*circle1, CIRCLE *circle2)
{
    return( FPge(circle_ar(circle1), circle_ar(circle2)) );
} /* circle_ge() */


/*----------------------------------------------------------
 *  "Arithmetic" operators on circles.
 *	circle_foo	returns foo as an object (pointer) that
 can be passed between languages.
 *	circle_xx	is an internal routine which returns the
 *			actual value.
 *---------------------------------------------------------*/

CIRCLE *circle_copy(CIRCLE *circle);

CIRCLE *
circle_copy(CIRCLE *circle)
{
    CIRCLE *result;

    if (!PointerIsValid(circle))
	return NULL;

    if (!PointerIsValid(result = PALLOCTYPE(CIRCLE)))
	elog(WARN, "Memory allocation failed, can't copy circle",NULL);

    memmove((char *) result, (char *) circle, sizeof(CIRCLE));
    return(result);
} /* circle_copy() */


/* circle_add_pt()
 * Translation operator.
 */
CIRCLE *
circle_add_pt(CIRCLE *circle, Point *point)
{
    CIRCLE *result;

    if (!PointerIsValid(circle) && !PointerIsValid(point))
	return(NULL);

    if (! PointerIsValid(result = circle_copy(circle)))
	elog(WARN, "Memory allocation failed, can't add circle",NULL);

    result->center.x += point->x;
    result->center.y += point->y;

    return(result);
} /* circle_add_pt() */

CIRCLE *
circle_sub_pt(CIRCLE *circle, Point *point)
{
    CIRCLE *result;

    if (!PointerIsValid(circle) && !PointerIsValid(point))
	return(NULL);

    if (! PointerIsValid(result = circle_copy(circle)))
	elog(WARN, "Memory allocation failed, can't subtract circle",NULL);

    result->center.x -= point->x;
    result->center.y -= point->y;

    return(result);
} /* circle_sub_pt() */


/* circle_mul_pt()
 * Rotation and scaling operators.
 */
CIRCLE *
circle_mul_pt(CIRCLE *circle, Point *point)
{
    CIRCLE *result;
    Point *p;

    if (!PointerIsValid(circle) && !PointerIsValid(point))
	return(NULL);

    if (! PointerIsValid(result = circle_copy(circle)))
	elog(WARN, "Memory allocation failed, can't multiply circle",NULL);

    p = point_mul( &circle->center, point);
    result->center.x = p->x;
    result->center.y = p->y;
    PFREE(p);
    result->radius *= HYPOT( point->x, point->y);

    return(result);
} /* circle_mul_pt() */

CIRCLE *
circle_div_pt(CIRCLE *circle, Point *point)
{
    CIRCLE *result;
    Point *p;

    if (!PointerIsValid(circle) && !PointerIsValid(point))
	return(NULL);

    if (! PointerIsValid(result = circle_copy(circle)))
	elog(WARN, "Memory allocation failed, can't add circle",NULL);

    p = point_div( &circle->center, point);
    result->center.x = p->x;
    result->center.y = p->y;
    PFREE(p);
    result->radius /= HYPOT( point->x, point->y);

    return(result);
} /* circle_div_pt() */


/*	circle_area	-	returns the area of the circle.
 */
double *circle_area(CIRCLE *circle)
{
    double *result;

    result = PALLOCTYPE(double);
    *result = circle_ar(circle);

    return(result);
}


/*	circle_diameter	-	returns the diameter of the circle.
 */
double *circle_diameter(CIRCLE *circle)
{
    double	*result;

    result = PALLOCTYPE(double);
    *result = (2*circle->radius);

    return(result);
}


/*	circle_radius	-	returns the radius of the circle.
 */
double *circle_radius(CIRCLE *circle)
{
    double	*result;

    result = PALLOCTYPE(double);
    *result = circle->radius;

    return(result);
}


/*	circle_distance	-	returns the distance between the
 *				  center points of two circlees.
 */
double *circle_distance(CIRCLE *circle1, CIRCLE *circle2)
{
    double	*result;

    result = PALLOCTYPE(double);
    *result = point_dt(&circle1->center,&circle2->center);

    return(result);
}


/*	circle_center	-	returns the center point of the circle.
 */
Point *circle_center(CIRCLE *circle)
{
    Point	*result;

    result = PALLOCTYPE(Point);
    result->x = circle->center.x;
    result->y = circle->center.y;

    return(result);
}


/*	circle_ar	-	returns the area of the circle.
 */
double circle_ar(CIRCLE *circle)
{
    return(PI*(circle->radius*circle->radius));
}


/*	circle_dt	-	returns the distance between the
 *			  center points of two circlees.
 */
double circle_dt(CIRCLE *circle1, CIRCLE *circle2)
{
    double	result;

    result = point_dt(&circle1->center,&circle2->center);

    return(result);
}


/*----------------------------------------------------------
 *  Conversion operators.
 *---------------------------------------------------------*/

CIRCLE *circle(Point *center, float8 *radius)
{
    CIRCLE *result;

    if (! (PointerIsValid(center) && PointerIsValid(radius)))
	return(NULL);

    if (!PointerIsValid(result = PALLOCTYPE(CIRCLE)))
      elog(WARN, "Memory allocation failed, can't convert point to circle",NULL);

    result->center.x = center->x;
    result->center.y = center->y;
    result->radius = *radius;

    return(result);
}

POLYGON *circle_poly(int npts, CIRCLE *circle)
{
    POLYGON *poly;
    int size;
    int i;
    double angle;

    if (!PointerIsValid(circle))
	return(NULL);

    if (FPzero(circle->radius) || (npts <= 2))
	  elog (WARN, "Unable to convert circle to polygon", NULL);

    size = offsetof(POLYGON, p[0]) + (sizeof(poly->p[0]) * npts);
    if (!PointerIsValid(poly = (POLYGON *) PALLOC(size)))
      elog(WARN, "Memory allocation failed, can't convert circle to polygon",NULL);

    memset((char *) poly, 0, size);	/* zero any holes */
    poly->size = size;
    poly->npts = npts;

    for (i=0;i<npts;i++) {
	angle = i*(2*PI/npts);
	poly->p[i].x = circle->center.x - (circle->radius*cos(angle));
	poly->p[i].y = circle->center.y + (circle->radius*sin(angle));
    };

    make_bound_box(poly);

    return(poly);
}

/*	poly_circle	- convert polygon to circle
 *
 * XXX This algorithm should use weighted means of line segments
 *  rather than straight average values of points - tgl 97/01/21.
 */
CIRCLE *poly_circle(POLYGON *poly)
{
    CIRCLE *circle;
    int i;

    if (!PointerIsValid(poly))
	return(NULL);

    if (poly->npts <= 2)
	  elog (WARN, "Unable to convert polygon to circle", NULL);

    if (!PointerIsValid(circle = PALLOCTYPE(CIRCLE)))
      elog(WARN, "Memory allocation failed, can't convert polygon to circle",NULL);

    circle->center.x = 0;
    circle->center.y = 0;
    circle->radius = 0;

    for (i=0;i<poly->npts;i++) {
	circle->center.x += poly->p[i].x;
	circle->center.y += poly->p[i].y;
    };
    circle->center.x /= poly->npts;
    circle->center.y /= poly->npts;

    for (i=0;i<poly->npts;i++) {
	circle->radius += point_dt( &poly->p[i], &circle->center);
    };
    circle->radius /= poly->npts;

    if (FPzero(circle->radius))
	  elog (WARN, "Unable to convert polygon to circle", NULL);

    return(circle);
}
