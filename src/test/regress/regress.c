/*
 * $Header: /cvsroot/pgsql/src/test/regress/regress.c,v 1.9 1997/08/19 21:40:56 momjian Exp $
 */

#include <float.h>		/* faked on sunos */
#include <stdio.h>
#include <string.h>   /* for memset() */

#include <postgres.h>

#include "utils/geo_decls.h"	/* includes <math.h> */
#include "executor/executor.h"  /* For GetAttributeByName */

#define P_MAXDIG 12
#define LDELIM		'('
#define RDELIM		')'
#define	DELIM		','

typedef void *TUPLE;

extern double *regress_dist_ptpath (Point *pt, PATH *path);
extern double *regress_path_dist (PATH *p1, PATH *p2);
extern PATH *poly2path (POLYGON *poly);
extern Point *interpt_pp (PATH *p1, PATH *p2);
extern void regress_lseg_construct (LSEG *lseg, Point *pt1, Point *pt2);
extern char overpaid (TUPLE tuple);
extern int boxarea (BOX *box);
extern char *reverse_c16 (char *string);

/*
** Distance from a point to a path 
*/
 double *
regress_dist_ptpath(pt, path)
    Point *pt;
    PATH *path;
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
	    regress_lseg_construct(&lseg, &path->p[i], &path->p[i+1]);
	    tmp = dist_ps(pt, &lseg);
	    if (i == 0 || *tmp < *result)
		*result = *tmp;
	    PFREE(tmp);

	}
	break;
    }
    return(result);
}

/* this essentially does a cartesian product of the lsegs in the
   two paths, and finds the min distance between any two lsegs */
 double *
regress_path_dist(p1, p2)
    PATH *p1;
    PATH *p2;
{
    double *min, *tmp;
    int i,j;
    LSEG seg1, seg2;

    regress_lseg_construct(&seg1, &p1->p[0], &p1->p[1]);
    regress_lseg_construct(&seg2, &p2->p[0], &p2->p[1]);
    min = lseg_distance(&seg1, &seg2);

    for (i = 0; i < p1->npts - 1; i++)
      for (j = 0; j < p2->npts - 1; j++)
       {
	   regress_lseg_construct(&seg1, &p1->p[i], &p1->p[i+1]);
	   regress_lseg_construct(&seg2, &p2->p[j], &p2->p[j+1]);

	   if (*min < *(tmp = lseg_distance(&seg1, &seg2)))
	     *min = *tmp;
	   PFREE(tmp);
       }

    return(min);
}

 PATH *
poly2path(poly)
    POLYGON *poly;
{
    int i;
    char *output = (char *)PALLOC(2*(P_MAXDIG + 1)*poly->npts + 64);
    char buf[2*(P_MAXDIG)+20];

    sprintf(output, "(1, %*d", P_MAXDIG, poly->npts);

    for (i=0; i<poly->npts; i++)
     {
	 sprintf(buf, ",%*g,%*g", P_MAXDIG, poly->p[i].x, P_MAXDIG, poly->p[i].y);
	 strcat(output, buf);
     }

    sprintf(buf, "%c", RDELIM);
    strcat(output, buf);
    return(path_in(output));
}

/* return the point where two paths intersect.  Assumes that they do. */
 Point *
interpt_pp(p1,p2)
    PATH *p1;
    PATH *p2;
{
        
    Point *retval;
    int i,j;
    LSEG seg1, seg2;
#if FALSE
    LINE *ln;
#endif
    bool found;  /* We've found the intersection */

    found = false;  /* Haven't found it yet */

    for (i = 0; i < p1->npts - 1 && !found; i++)
      for (j = 0; j < p2->npts - 1 && !found; j++)
       {
           regress_lseg_construct(&seg1, &p1->p[i], &p1->p[i+1]);
           regress_lseg_construct(&seg2, &p2->p[j], &p2->p[j+1]);
           if (lseg_intersect(&seg1, &seg2)) found = true;
       }

#if FALSE
    ln = line_construct_pp(&seg2.p[0], &seg2.p[1]);
    retval = interpt_sl(&seg1, ln);
#endif
    retval = lseg_interpt( &seg1, &seg2);
    
    return(retval);
}


/* like lseg_construct, but assume space already allocated */
 void
regress_lseg_construct(lseg, pt1, pt2)
    LSEG *lseg;
    Point *pt1;
    Point *pt2;
{
    lseg->p[0].x = pt1->x;
    lseg->p[0].y = pt1->y;
    lseg->p[1].x = pt2->x;
    lseg->p[1].y = pt2->y;
    lseg->m = point_sl(pt1, pt2);
}


 char overpaid(tuple)
    TUPLE tuple;
{
    bool isnull;
    long salary;

    salary = (long)GetAttributeByName(tuple, "salary", &isnull);
    return(salary > 699);
}

/* New type "widget"
 * This used to be "circle", but I added circle to builtins,
 *  so needed to make sure the names do not collide. - tgl 97/04/21
 */

typedef struct {
	Point	center;
	double	radius;
} WIDGET;

 WIDGET *widget_in (char *str);
 char *widget_out (WIDGET *widget);
 int pt_in_widget (Point *point, WIDGET *widget);

#define NARGS	3

 WIDGET *
widget_in(str)
char	*str;
{
	char	*p, *coord[NARGS], buf2[1000];
	int	i;
	WIDGET	*result;

	if (str == NULL)
		return(NULL);
	for (i = 0, p = str; *p && i < NARGS && *p != RDELIM; p++)
		if (*p == ',' || (*p == LDELIM && !i))
			coord[i++] = p + 1;
	if (i < NARGS - 1)
		return(NULL);
	result = (WIDGET *) palloc(sizeof(WIDGET));
	result->center.x = atof(coord[0]);
	result->center.y = atof(coord[1]);
	result->radius = atof(coord[2]);

	sprintf(buf2, "widget_in: read (%f, %f, %f)\n", result->center.x,
	result->center.y,result->radius);
	return(result);
}

 char *
widget_out(widget)
    WIDGET	*widget;
{
    char	*result;

    if (widget == NULL)
	return(NULL);

    result = (char *) palloc(60);
    sprintf(result, "(%g,%g,%g)",
		   widget->center.x, widget->center.y, widget->radius);
    return(result);
}

 int
pt_in_widget(point, widget)
	Point	*point;
	WIDGET	*widget;
{
	extern double	point_dt();

	return( point_dt(point, &widget->center) < widget->radius );
}

#define ABS(X) ((X) > 0 ? (X) : -(X))

 int
boxarea(box)

BOX *box;

{
	int width, height;

	width  = ABS(box->high.x - box->low.x);
	height = ABS(box->high.y - box->low.y);
	return (width * height);
}

 char *
reverse_c16(string)
    char *string;
{
    register i;
    int len;
    char *new_string;

    if (!(new_string = palloc(16))) {
	fprintf(stderr, "reverse_c16: palloc failed\n");
	return(NULL);
    }
    memset(new_string, 0, 16);
    for (i = 0; i < 16 && string[i]; ++i)
	;
    if (i == 16 || !string[i])
	--i;
    len = i;
    for (; i >= 0; --i)
	new_string[len-i] = string[i];
    return(new_string);
}
