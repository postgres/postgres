/*-------------------------------------------------------------------------
 *
 * geo-ops.c--
 *    2D geometric operations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/geo-ops.c,v 1.2 1996/07/22 21:56:01 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <float.h>	/* faked on sunos */
#include <stdio.h>	/* for sprintf proto, etc. */
#include <string.h>

#include "utils/geo-decls.h"
#include "utils/elog.h"
#include "utils/palloc.h"

#define LDELIM		'('
#define RDELIM		')'
#define	DELIM		','
#define BOXNARGS	4
#define	LSEGNARGS	4
#define	POINTNARGS	2

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
 *	str:	input string "(f8, f8, f8, f8)"
 */
BOX *box_in(char *str)
{
    double	tmp;
    char	*p, *coord[BOXNARGS];
    int	i;
    BOX	*result;
    
    if (str == NULL)
	elog (WARN," Bad (null) box external representation");
    
    if ((p = (char *)strchr(str, LDELIM)) == (char *)NULL)
	elog (WARN, "Bad box external representation '%s'",str);
    for (i = 0, p = str; *p && i < BOXNARGS && *p != RDELIM; p++)
	if (*p == DELIM || (*p == LDELIM && !i))
	    coord[i++] = p + 1;
    if (i < BOXNARGS - 1)
	elog (WARN, "Bad box external representation '%s'", str);
    result = PALLOCTYPE(BOX);
    result->xh = atof(coord[0]);
    result->yh = atof(coord[1]);
    result->xl = atof(coord[2]);
    result->yl = atof(coord[3]);
    if (result->xh < result->xl) {
	tmp = result->xh;
	result->xh = result->xl;
	result->xl = tmp;
    }
    if (result->yh < result->yl) {
	tmp = result->yh;
	result->yh = result->yl;
	result->yl = tmp;
    }
    
    return(result);
}

/*	box_out	-	convert a box to external form.
 */
char *box_out(BOX *box)
{
    char	*result;
    
    if (box == NULL)
	return(NULL);
    result = (char *)PALLOC(80);
    (void) sprintf(result, "(%G,%G,%G,%G)",
		   box->xh, box->yh, box->xl, box->yl);
    
    return(result);
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
    double	tmp;
    
    result->xh = x1;
    result->xl = x2;
    result->yh = y1;
    result->yl = y2;
    if (result->xh < result->xl) {
	tmp = result->xh;
	result->xh = result->xl;
	result->xl = tmp;
    }
    if (result->yh < result->yl) {
	tmp = result->yh;
	result->yh = result->yl;
	result->yl = tmp;
    }
    
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
long box_same(BOX *box1, BOX *box2)
{
    return((box1->xh == box2->xh && box1->xl == box2->xl) &&
	   (box1->yh == box2->yh && box1->yl == box2->yl));
}

/*	box_overlap	-	does box1 overlap box2?
 */
long box_overlap(BOX *box1, BOX *box2)
{
    return(((box1->xh >= box2->xh && box1->xl <= box2->xh) ||
	    (box2->xh >= box1->xh && box2->xl <= box1->xh)) &&
	   ((box1->yh >= box2->yh && box1->yl <= box2->yh) ||
	    (box2->yh >= box1->yh && box2->yl <= box1->yh)) );
}

/*	box_overleft	-	is the right edge of box1 to the left of
 *				the right edge of box2?
 *
 *	This is "less than or equal" for the end of a time range,
 *	when time ranges are stored as rectangles.
 */
long box_overleft(BOX *box1, BOX *box2)
{
    return(box1->xh <= box2->xh);
}

/*	box_left	-	is box1 strictly left of box2?
 */
long box_left(BOX *box1, BOX *box2)
{
    return(box1->xh < box2->xl);
}

/*	box_right	-	is box1 strictly right of box2?
 */
long box_right(BOX *box1, BOX *box2)
{
    return(box1->xl > box2->xh);
}

/*	box_overright	-	is the left edge of box1 to the right of
 *				the left edge of box2?
 *
 *	This is "greater than or equal" for time ranges, when time ranges
 *	are stored as rectangles.
 */
long box_overright(BOX *box1, BOX *box2)
{
    return(box1->xl >= box2->xl);
}

/*	box_contained	-	is box1 contained by box2?
 */
long box_contained(BOX *box1, BOX *box2)
{
    return((box1->xh <= box2->xh && box1->xl >= box2->xl &&
	    box1->yh <= box2->yh && box1->yl >= box2->yl));
}

/*	box_contain	-	does box1 contain box2?
 */
long box_contain(BOX *box1, BOX *box2)
{
    return((box1->xh >= box2->xh && box1->xl <= box2->xl &&
	    box1->yh >= box2->yh && box1->yl <= box2->yl));
}


/*	box_positionop	-
 *		is box1 entirely {above, below } box2?
 */
long box_below(BOX *box1, BOX *box2)
{
    return( box1->yh <= box2->yl );
}

long box_above(BOX *box1, BOX *box2)
{
    return( box1->yl >= box2->yh );
}


/*	box_relop	-	is area(box1) relop area(box2), within
 *			  	our accuracy constraint?
 */
long box_lt(BOX *box1, BOX *box2)
{
    return( FPlt(box_ar(box1), box_ar(box2)) );
}

long box_gt(BOX *box1, BOX *box2)
{
    return( FPgt(box_ar(box1), box_ar(box2)) );
}

long box_eq(BOX *box1, BOX *box2)
{
    return( FPeq(box_ar(box1), box_ar(box2)) );
}

long box_le(BOX	*box1, BOX *box2)
{
    return( FPle(box_ar(box1), box_ar(box2)) );
}

long box_ge(BOX	*box1, BOX *box2)
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
    *result = box->xh - box->xl;
    
    return(result);
}


/*	box_height	-	returns the height of the box 
 *				  (vertical magnitude).
 */
double *box_height(BOX *box)
{
    double	*result;
    
    result = PALLOCTYPE(double);
    *result = box->yh - box->yl;
    
    return(result);
}


/*	box_distance	-	returns the distance between the
 *				  center points of two boxes.
 */
double *box_distance(BOX *box1, BOX *box2)
{
    double	*result;
    Point	*box_center(), *a, *b;
    
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
    result->x = (box->xh + box->xl) / 2.0;
    result->y = (box->yh + box->yl) / 2.0;
    
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
    return( box->xh - box->xl );
}


/*	box_ht	-	returns the height of the box 
 *				  (vertical magnitude).
 */
double box_ht(BOX *box)
{
    return( box->yh - box->yl );
}


/*	box_dt	-	returns the distance between the
 *			  center points of two boxes.
 */
double box_dt(BOX *box1, BOX *box2)
{
    double	result;
    Point	*box_center(),
    *a, *b;
    
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
    long	box_overlap();
    
    if (! box_overlap(box1,box2))
	return(NULL);
    result = PALLOCTYPE(BOX);
    result->xh = Min(box1->xh, box2->xh);
    result->xl = Max(box1->xl, box2->xl);
    result->yh = Min(box1->yh, box2->yh);
    result->yl = Max(box1->yl, box2->yl);
    
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
    
    p1.x = box->xh;
    p1.y = box->yh;
    p2.x = box->xl;
    p2.y = box->yl;
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

long line_intersect(LINE *l1, LINE *l2)
{
    return( ! line_parallel(l1, l2) );
}

long line_parallel(LINE *l1, LINE *l2)
{
    return( FPeq(l1->m, l2->m) );
}

long line_perp(LINE *l1, LINE *l2)
{
    if (l1->m)
	return( FPeq(l2->m / l1->m, -1.0) );
    else if (l2->m)
	return( FPeq(l1->m / l2->m, -1.0) );
    return(1);	/* both 0.0 */
}

long line_vertical(LINE *line)
{
    return( FPeq(line->A, -1.0) && FPzero(line->B) );
}

long line_horizontal(LINE *line)
{
    return( FPzero(line->m) );
}


long line_eq(LINE *l1, LINE *l2)
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

#define	PATHALLOCSIZE(N) \
    (long) ((unsigned) (sizeof(PATH) + \
			(((N)-1) > 0 ? ((N)-1) : 0) \
			* sizeof(Point)))

/*----------------------------------------------------------
 *  String to path / path to string conversion.
 *	External format: 
 *		"(closed, npts, xcoord, ycoord,... )"
 *---------------------------------------------------------*/

PATH *path_in(char *str)
{
    double	coord;
    long	field[2];
    char	*s;
    int	ct, i;
    PATH	*result;
    long	pathsize;
    
    if (str == NULL)
	elog(WARN, "Bad (null) path external representation");
    
    /* read the path header information */
    for (i = 0, s = str; *s && i < 2 && *s != RDELIM; ++s)
	if (*s == DELIM || (*s == LDELIM && !i))
	    field[i++] = atol(s + 1);
    if (i < 1)
	elog(WARN, "Bad path external representation '%s'", str);
    pathsize = PATHALLOCSIZE(field[1]);
    result = (PATH *)palloc(pathsize);
    result->length = pathsize;
    result->closed = field[0];
    result->npts =  field[1];
    
    /* read the path points */
    
    ct = result->npts * 2;	/* two coords for every point */
    for (i = 0;
	 *s && i < ct && *s != RDELIM; 
	 ++s) {
	if (*s == ',') {
	    coord = atof(s + 1);
	    if (i % 2)
		(result->p[i/2]).y = coord;
	    else
		(result->p[i/2]).x = coord;
	    ++i;
	}
    }
    if (i % 2 || i < --ct) {
	PFREE(result);
	elog(WARN, "Bad path external representation '%s'", str);
    } 
    
    return(result);
}


char *path_out(PATH *path)
{
    char 		buf[BUFSIZ + 20000], *result, *s;
    int		i;
    char	tmp[64];
    
    if (path == NULL)
	return(NULL);
    (void) sprintf(buf,"%c%d,%d", LDELIM, 
		   path->closed, path->npts);
    s = buf + strlen(buf);
    for (i = 0; i < path->npts; ++i) {
	(void) sprintf(tmp, ",%G,%G", 
		       path->p[i].x, path->p[i].y);
	(void) strcpy(s, tmp);
	s += strlen(tmp);
    }
    *s++ = RDELIM;
    *s = '\0';
    result = (char *)PALLOC(strlen(buf) + 1);
    (void) strcpy(result, buf);
    
    return(result);
}


/*----------------------------------------------------------
 *  Relational operators.
 *	These are based on the path cardinality, 
 *	as stupid as that sounds.
 *
 *	Better relops and access methods coming soon.
 *---------------------------------------------------------*/

long path_n_lt(PATH *p1, PATH *p2)
{
    return( (p1->npts < p2->npts ) );
}

long path_n_gt(PATH *p1, PATH *p2)
{
    return( (p1->npts > p2->npts ) );
}

long path_n_eq(PATH *p1, PATH *p2)
{
    return( (p1->npts == p2->npts) );
}

long path_n_le(PATH *p1, PATH *p2)
{
    return( (p1->npts <= p2->npts ) );
}

long path_n_ge(PATH *p1, PATH *p2)
{
    return( (p1->npts >= p2->npts ) );
}

/* path_inter -
 *	Does p1 intersect p2 at any point?
 *	Use bounding boxes for a quick (O(n)) check, then do a 
 *	O(n^2) iterative edge check.
 */
long path_inter(PATH *p1, PATH *p2)
{
    BOX	b1, b2;
    int	i, j;
    LSEG seg1, seg2;
    
    b1.xh = b1.yh = b2.xh = b2.yh = (double)DBL_MAX;
    b1.xl = b1.yl = b2.xl = b2.yl = -(double)DBL_MAX;
    for (i = 0; i < p1->npts; ++i) {
	b1.xh = Max(p1->p[i].x, b1.xh);
	b1.yh = Max(p1->p[i].y, b1.yh);
	b1.xl = Min(p1->p[i].x, b1.xl);
	b1.yl = Min(p1->p[i].y, b1.yl);
    }
    for (i = 0; i < p2->npts; ++i) {
	b2.xh = Max(p2->p[i].x, b2.xh);
	b2.yh = Max(p2->p[i].y, b2.yh);
	b2.xl = Min(p2->p[i].x, b2.xl);
	b2.yl = Min(p2->p[i].y, b2.yl);
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
    double *min, *tmp;
    int i,j;
    LSEG seg1, seg2;
    
    statlseg_construct(&seg1, &p1->p[0], &p1->p[1]);
    statlseg_construct(&seg2, &p2->p[0], &p2->p[1]);
    min = lseg_distance(&seg1, &seg2);
    
    for (i = 0; i < p1->npts - 1; i++)
	for (j = 0; j < p2->npts - 1; j++)
	    {
		statlseg_construct(&seg1, &p1->p[i], &p1->p[i+1]);
		statlseg_construct(&seg2, &p2->p[j], &p2->p[j+1]);
		
		if (*min < *(tmp = lseg_distance(&seg1, &seg2)))
		    *min = *tmp;
		PFREE(tmp);
	    }
    
    return(min);
}


/*----------------------------------------------------------
 *  "Arithmetic" operations.
 *---------------------------------------------------------*/

double *path_length(PATH *path)
{
    double	*result;
    int	ct, i;
    
    result = PALLOCTYPE(double);
    ct = path->npts - 1;
    for (i = 0; i < ct; ++i)
	*result += point_dt(&path->p[i], &path->p[i+1]);
    
    return(result);
}



double path_ln(PATH *path)
{
    double	result;
    int	ct, i;
    
    ct = path->npts - 1;
    for (result = i = 0; i < ct; ++i)
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
 *	External form:	"(x, y)"
 *---------------------------------------------------------*/

Point *point_in(char *str)
{
    char	*coord[POINTNARGS], *p, *r;
    int	i;
    Point	*result;
    
    if (str == NULL)
	elog(WARN, "Bad (null) point external representation");
    
    if ((p = (char *)strchr(str, LDELIM)) == (char *)NULL)
	elog (WARN, "Bad point external representation '%s'",str);
    for (i = 0, p++; *p && i < POINTNARGS-1 && *p != RDELIM; p = r+1)
	if ((r = (char *)strchr(p, DELIM)) == (char *)NULL)
	    elog (WARN, "Bad point external representation '%s'",str);
	else	
	    coord[i++] = p;
    if ((r = (char *)strchr(p, RDELIM)) == (char *)NULL)
	elog (WARN, "Bad point external representation '%s'",str);
    coord[i++] = p;
    
    if (i < POINTNARGS - 1)
	elog(WARN, "Bad point external representation '%s'",str);
    result = PALLOCTYPE(Point);
    result->x = atof(coord[0]);
    result->y = atof(coord[1]);
    return(result);
}

char *point_out(Point *pt)
{
    char	*result;
    
    if (pt == NULL)
	return(NULL);
    result = (char *)PALLOC(40);
    (void) sprintf(result, "(%G,%G)", pt->x, pt->y);
    return(result);
}


Point *point_construct(double x, double y)
{
    Point	*result;
    
    result = PALLOCTYPE(Point);
    result->x = x;
    result->y = y;
    return(result);
}


Point *point_copy(Point *pt)
{
    Point	*result;
    
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

long point_left(Point *pt1, Point *pt2)
{
    return( FPlt(pt1->x, pt2->x) );
}

long point_right(Point *pt1, Point *pt2)
{
    return( FPgt(pt1->x, pt2->x) );
}

long point_above(Point *pt1, Point *pt2)
{
    return( FPgt(pt1->y, pt2->y) );
}

long point_below(Point *pt1, Point *pt2)
{
    return( FPlt(pt1->y, pt2->y) );
}

long point_vert(Point *pt1, Point *pt2)
{
    return( FPeq( pt1->x, pt2->x ) );
}

long point_horiz(Point *pt1, Point *pt2)
{
    return( FPeq( pt1->y, pt2->y ) );
}

long point_eq(Point *pt1, Point *pt2)
{
    return( point_horiz(pt1, pt2) && point_vert(pt1, pt2) );
}

/*----------------------------------------------------------
 *  "Arithmetic" operators on points.
 *---------------------------------------------------------*/

long pointdist(Point *p1, Point *p2)
{
    long result;
    
    result = point_dt(p1, p2);
    return(result);
}

double *point_distance(Point *pt1, Point *pt2)
{
    double	*result;
    
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
    double	*result;
    
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
 *	External form:	"(id, info, x1, y1, x2, y2)"
 *---------------------------------------------------------*/

LSEG *lseg_in(char *str)
{
    char	*coord[LSEGNARGS], *p;
    int	i;
    LSEG	*result;
    
    if (str == NULL)
	elog (WARN," Bad (null) box external representation");
    
    if ((p = (char *)strchr(str, LDELIM)) == (char *)NULL)
	elog (WARN, "Bad lseg external representation '%s'",str);
    for (i = 0, p = str; *p && i < LSEGNARGS && *p != RDELIM; p++)
	if (*p == DELIM || (*p == LDELIM && !i))
	    coord[i++] = p + 1;
    if (i < LSEGNARGS - 1)
	elog (WARN, "Bad lseg external representation '%s'", str);
    result = PALLOCTYPE(LSEG);
    result->p[0].x = atof(coord[0]);
    result->p[0].y = atof(coord[1]);
    result->p[1].x = atof(coord[2]);
    result->p[1].y = atof(coord[3]);
    result->m = point_sl(&result->p[0], &result->p[1]);
    
    return(result);
}


char *lseg_out(LSEG *ls)
{
    char	*result;
    
    if (ls == NULL)
	return(NULL);
    result = (char *)PALLOC(80);
    (void) sprintf(result, "(%G,%G,%G,%G)",
		   ls->p[0].x, ls->p[0].y, ls->p[1].x, ls->p[1].y);
    return(result);
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
long lseg_intersect(LSEG *l1, LSEG *l2)
{
    LINE *ln;
    Point *interpt;
    long retval;
    
    ln = line_construct_pp(&l2->p[0], &l2->p[1]);
    interpt = interpt_sl(l1, ln);
    
    if (interpt != NULL && on_ps(interpt, l2)) /* interpt on l1 and l2 */
	retval = 1;
    else retval = 0;
    if (interpt != NULL) PFREE(interpt);
    PFREE(ln);
    return(retval);
}

long lseg_parallel(LSEG *l1, LSEG *l2)
{
    return( FPeq(l1->m, l2->m) );
}

long lseg_perp(LSEG *l1, LSEG *l2)
{
    if (! FPzero(l1->m))
	return( FPeq(l2->m / l1->m, -1.0) );
    else if (! FPzero(l2->m))
	return( FPeq(l1->m / l2->m, -1.0) );
    return(0);	/* both 0.0 */
}

long lseg_vertical(LSEG *lseg)
{
    return( FPeq(lseg->p[0].x, lseg->p[1].x) );
}

long lseg_horizontal(LSEG *lseg)
{
    return( FPeq(lseg->p[0].y, lseg->p[1].y) );
}


long lseg_eq(LSEG *l1, LSEG *l2)
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
    double	*d, *result;
    
    result = PALLOCTYPE(double);
    if (lseg_intersect(l1, l2)) {
	*result = 0.0;
	return(result);
    }
    *result = (double)DBL_MAX;
    d = dist_ps(&l1->p[0], l2);
    *result = Min(*result, *d);
    PFREE(d);
    d = dist_ps(&l1->p[1], l2);
    *result = Min(*result, *d);
    PFREE(d);
    d = dist_ps(&l2->p[0], l1);
    *result = Min(*result, *d);
    PFREE(d);
    d = dist_ps(&l2->p[1], l1);
    *result = Min(*result, *d);
    PFREE(d);
    
    return(result);
}

/* distance between l1, l2 */
double lseg_dt(LSEG *l1, LSEG *l2)
{
    double	*d, result;
    
    if (lseg_intersect(l1, l2))
	return(0.0);
    result = (double)DBL_MAX;
    d = dist_ps(&l1->p[0], l2);
    result = Min(result, *d);
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
    case 0:
	result = PALLOCTYPE(double);
	*result = Abs((double) DBL_MAX);	/* +infinity */
	break;
    case 1:
	result = point_distance(pt, &path->p[0]);
	break;
    default:
	/*
	 * the distance from a point to a path is the smallest distance
	 * from the point to any of its constituent segments.
	 */
	Assert(path->npts > 1);
	result = PALLOCTYPE(double);
	for (i = 0; i < path->npts - 1; ++i) {
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
    double	*result;
    
    if (inter_sl(lseg, line)) {
	result = PALLOCTYPE(double);
	*result = 0.0;
    } else	/* parallel */
	result = dist_pl(&lseg->p[0], line);
    
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
long on_pl(Point *pt, LINE *line)
{
    return( FPzero(line->A * pt->x + line->B * pt->y + line->C) );
}


/* on_ps -
 *	Determine colinearity by detecting a triangle inequality.
 */
long on_ps(Point *pt, LSEG *lseg)
{
    return( FPeq (point_dt(pt, &lseg->p[0]) + point_dt(pt, &lseg->p[1]),
            point_dt(&lseg->p[0], &lseg->p[1])) );
}

long on_pb(Point *pt, BOX *box)
{
    return( pt->x <= box->xh && pt->x >= box->xl &&
	   pt->y <= box->yh && pt->y >= box->yl );
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

long on_ppath(Point *pt, PATH *path)
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


long on_sl(LSEG *lseg, LINE *line)
{
    return( on_pl(&lseg->p[0], line) && on_pl(&lseg->p[1], line) );
}

long on_sb(LSEG *lseg, BOX *box)
{
    return( on_pb(&lseg->p[0], box) && on_pb(&lseg->p[1], box) );
}

/*---------------------------------------------------------------------
 *	inter_
 *		Whether one object intersects another.
 *-------------------------------------------------------------------*/

long inter_sl(LSEG *lseg, LINE *line)
{
    Point	*tmp;

    tmp = interpt_sl(lseg, line);
    if (tmp) {
	PFREE(tmp);
	return(1);
    }
    return(0);
}

long inter_sb(LSEG *lseg, BOX *box)
{
    return(0);
}

long inter_lb(LINE *line, BOX *box)
{
    return(0);
}

/*------------------------------------------------------------------
 * The following routines define a data type and operator class for
 * POLYGONS .... Part of which (the polygon's bounding box is built on 
 * top of the BOX data type.
 *
 * make_bound_box - create the bounding box for the input polygon
 *------------------------------------------------------------------*/

/* Maximum number of output digits printed */
#define P_MAXDIG 12

/*---------------------------------------------------------------------
 * Make the smallest bounding box for the given polygon.
 *---------------------------------------------------------------------*/
void make_bound_box(POLYGON *poly)
{
    double x1,y1,x2,y2;
    int npts = poly->npts;
    
    if (npts > 0) {
	x1 = poly_min((double *)poly->pts, npts);
	x2 = poly_max((double *)poly->pts, npts);
	y1 = poly_min(((double *)poly->pts)+npts, npts),
	y2 = poly_max(((double *)poly->pts)+npts, npts);
	box_fill(&(poly->boundbox), x1, x2, y1, y2); 
    }
}

/*------------------------------------------------------------------
 * polygon_in - read in the polygon from a string specification
 *              the string is of the form "(f8,f8,f8,f8,...,f8)"
 *------------------------------------------------------------------*/
POLYGON *poly_in(char *s)
{
    POLYGON *poly;
    long points;
    double *xp, *yp, strtod();
    int i, size;
    
    if((points = poly_pt_count(s, ',')) < 0)
	elog(WARN, "Bad polygon external representation '%s'", s);
    
    size = offsetof(POLYGON, pts[0]) + 2 * sizeof(double) * points;
    poly = (POLYGON *) PALLOC(size);
    memset((char *) poly, 0, size);	/* zero any holes */
    
    if (!PointerIsValid(poly))
	elog(WARN, "Memory allocation failed, can't input polygon");
    
    poly->npts = points;
    poly->size = size;
    
    /* Store all x coords followed by all y coords */
    xp = (double *) &(poly->pts[0]);
    yp = (double *) (poly->pts + points*sizeof(double));
    
    s++;				/* skip LDELIM */
    
    for (i=0; i<points; i++,xp++,yp++)
	{
	    *xp = strtod(s, &s);
	    s++;					/* skip delimiter */
	    *yp = strtod(s, &s);
	    s++;					/* skip delimiter */
	}
    make_bound_box(poly);
    return (poly);
}

/*-------------------------------------------------------------
 * poly_pt_count - count the number of points specified in the
 *                 polygon.
 *-------------------------------------------------------------*/
long poly_pt_count(char *s, char delim)
{
    long total = 0;
    
    if (*s++ != LDELIM)		/* no left delimeter */
	return (long) -1;
    
    while (*s && (*s != RDELIM))
	{
	    while (*s && (*s != delim))
		s++;
	    total++;	/* found one */
	    if (*s)
		s++;	/* bump s past the delimiter */
	}
    
    /* if there was no right delimiter OR an odd number of points */
    
    if ((*(s-1) != RDELIM) || ((total%2) != 0))
	return (long) -1;
    
    return (total/2);
}

/*---------------------------------------------------------------
 * poly_out - convert internal POLYGON representation to the 
 *            character string format "(f8,f8,f8,f8,...f8)"
 *---------------------------------------------------------------*/
char *poly_out(POLYGON *poly)
{
    int i;
    double *xp, *yp;
    char *output, *outptr;
    
    /*-----------------------------------------------------
     * Get enough space for "(f8,f8,f8,f8,...,f8)"
     * which P_MAXDIG+1 for each coordinate plus 2
     * for parens and 1 for the null
     *-----------------------------------------------------*/
    output = (char *)PALLOC(2*(P_MAXDIG+1)*poly->npts + 3);
    outptr = output;
    
    if (!output)
	elog(WARN, "Memory allocation failed, can't output polygon");
    
    *outptr++ = LDELIM;
    
    xp = (double *) poly->pts;
    yp = (double *) (poly->pts + (poly->npts * sizeof(double)));
    
    sprintf(outptr, "%*g,%*g", P_MAXDIG, *xp++, P_MAXDIG, *yp++);
    outptr += (2*P_MAXDIG + 1);
    
    for (i=1; i<poly->npts; i++,xp++,yp++)
	{
	    sprintf(outptr, ",%*g,%*g", P_MAXDIG, *xp, P_MAXDIG, *yp);
	    outptr += 2*(P_MAXDIG + 1);
	}
    *outptr++ = RDELIM;
    *outptr = '\0';
    return (output);
}

/*-------------------------------------------------------
 * Find the largest coordinate out of n coordinates
 *-------------------------------------------------------*/
double poly_max(double *coords, int ncoords)
{
    double max;
    
    max = *coords++;
    ncoords--;
    while (ncoords--)
	{
	    if (*coords > max)
		max = *coords;
	    coords++;
	}
    return max;
}

/*-------------------------------------------------------
 * Find the smallest coordinate out of n coordinates
 *-------------------------------------------------------*/
double poly_min(double *coords, int ncoords)
{
    double min;
    
    min = *coords++;
    ncoords--;
    while (ncoords--)
	{
	    if (*coords < min)
		min = *coords;
	    coords++;
	}
    return min;
}

/*-------------------------------------------------------
 * Is polygon A strictly left of polygon B? i.e. is
 * the right most point of A left of the left most point
 * of B?
 *-------------------------------------------------------*/
long poly_left(POLYGON *polya, POLYGON *polyb)
{
    double right, left;
    
    if (polya->npts > 0)
	right = poly_max((double *)polya->pts, polya->npts);
    else
	right = polya->boundbox.xh;
    if (polyb->npts > 0)
	left = poly_min((double *)polyb->pts, polyb->npts);
    else
	left = polyb->boundbox.xl;
    
    return (right < left);
}

/*-------------------------------------------------------
 * Is polygon A overlapping or left of polygon B? i.e. is
 * the left most point of A left of the right most point
 * of B?
 *-------------------------------------------------------*/
long poly_overleft(POLYGON *polya, POLYGON *polyb)
{
    double left, right;
    
    if (polya->npts > 0)
	left = poly_min((double *)polya->pts, polya->npts);
    else
	left = polya->boundbox.xl;
    if (polyb->npts > 0)
	right = poly_max((double *)polyb->pts, polyb->npts);
    else
	right = polyb->boundbox.xh;
    
    return (left <= right);
}

/*-------------------------------------------------------
 * Is polygon A strictly right of polygon B? i.e. is
 * the left most point of A right of the right most point
 * of B?
 *-------------------------------------------------------*/
long poly_right(POLYGON *polya, POLYGON *polyb)
{
    double right, left;
    
    if (polya->npts > 0)
	left = poly_min((double *)polya->pts, polya->npts);
    else
	left = polya->boundbox.xl;
    if (polyb->npts > 0)
	right = poly_max((double *)polyb->pts, polyb->npts);
    else
	right = polyb->boundbox.xh;
    
    return (left > right);
}

/*-------------------------------------------------------
 * Is polygon A overlapping or right of polygon B? i.e. is
 * the right most point of A right of the left most point
 * of B?
 *-------------------------------------------------------*/
long poly_overright(POLYGON *polya, POLYGON *polyb)
{
    double right, left;
    
    if (polya->npts > 0)
	right = poly_max((double *)polya->pts, polya->npts);
    else
	right = polya->boundbox.xh;
    if (polyb->npts > 0)
	left = poly_min((double *)polyb->pts, polyb->npts);
    else
	left = polyb->boundbox.xl;
    
    return (right > left);
}

/*-------------------------------------------------------
 * Is polygon A the same as polygon B? i.e. are all the
 * points the same?
 *-------------------------------------------------------*/
long poly_same(POLYGON *polya, POLYGON *polyb)
{
    int i;
    double *axp, *bxp; /* point to x coordinates for a and b */
    
    if (polya->npts != polyb->npts)
	return 0;
    
    axp = (double *)polya->pts;
    bxp = (double *)polyb->pts;
    
    for (i=0; i<polya->npts; axp++, bxp++, i++)
	{
	    if (*axp != *bxp)
		return 0;
	}
    return 1;
}

/*-----------------------------------------------------------------
 * Determine if polygon A overlaps polygon B by determining if
 * their bounding boxes overlap.
 *-----------------------------------------------------------------*/
long poly_overlap(POLYGON *polya, POLYGON *polyb)
{
    return box_overlap(&(polya->boundbox), &(polyb->boundbox));
}

/*-----------------------------------------------------------------
 * Determine if polygon A contains polygon B by determining if A's
 * bounding box contains B's bounding box.
 *-----------------------------------------------------------------*/
long poly_contain(POLYGON *polya, POLYGON *polyb)
{
    return box_contain(&(polya->boundbox), &(polyb->boundbox));
}

/*-----------------------------------------------------------------
 * Determine if polygon A is contained by polygon B by determining 
 * if A's bounding box is contained by B's bounding box.
 *-----------------------------------------------------------------*/
long poly_contained(POLYGON *polya, POLYGON *polyb)
{
    return box_contained(&(polya->boundbox), &(polyb->boundbox));
}
