/*-------------------------------------------------------------------------
 *
 * geo-decls.h--
 *    Declarations for various 2D constructs.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geo-decls.h,v 1.1.1.1 1996/07/09 06:22:02 scrappy Exp $
 *
 * NOTE
 *    These routines do *not* use the float types from adt/.
 *
 *    XXX These routines were not written by a numerical analyst.
 *
 *-------------------------------------------------------------------------
 */
#ifndef	GEO_DECLS_H
#define	GEO_DECLS_H

/*#ifndef FmgrIncluded -- seems like always included. (it's FMgrIncluded) AY */

/*--------------------------------------------------------------------
 *	Useful floating point utilities and constants.
 *-------------------------------------------------------------------*/

#include <math.h>
#include "c.h"

#define	EPSILON			1.0E-06

#define	FPzero(A)		(fabs(A) <= EPSILON)
#define	FPeq(A,B)		(fabs((A) - (B)) <= EPSILON)
#define	FPlt(A,B)		((B) - (A) > EPSILON)
#define	FPle(A,B)		((A) - (B) <= EPSILON)
#define	FPgt(A,B)		((A) - (B) > EPSILON)
#define	FPge(A,B)		((B) - (A) <= EPSILON)

#define	HYPOT(A, B)		sqrt((A) * (A) + (B) * (B))

/*--------------------------------------------------------------------
 *	Memory management.
 *-------------------------------------------------------------------*/

#define	PALLOC(SIZE)		palloc(SIZE)
#define	PFREE(P)		pfree(P)
#define	PALLOCTYPE(TYPE)	(TYPE *) PALLOC(sizeof(TYPE))

/*#endif !FmgrIncluded */

/*---------------------------------------------------------------------
 *	Point	-	(x,y)
 *-------------------------------------------------------------------*/
typedef struct {
	double	x, y;
} Point;


/*---------------------------------------------------------------------
 *	LSEG	- 	A straight line, specified by endpoints.
 *-------------------------------------------------------------------*/
typedef	struct {
	Point	p[2];

	double	m;	/* precomputed to save time, not in tuple */
} LSEG;


/*---------------------------------------------------------------------
 *	PATH	- 	Specified by vertex points.
 *-------------------------------------------------------------------*/
typedef	struct {
	int32	length;	/* XXX varlena */
	int32	npts;
	int32	closed;	/* is this a closed polygon? */
	int32	dummy;	/* padding to make it double align */
	Point	p[1];	/* variable length array of POINTs */
} PATH;


/*---------------------------------------------------------------------
 *	LINE	-	Specified by its general equation (Ax+By+C=0).
 *			If there is a y-intercept, it is C, which
 *			 incidentally gives a freebie point on the line
 *			 (if B=0, then C is the x-intercept).
 *			Slope m is precalculated to save time; if
 *			 the line is not vertical, m == A.
 *-------------------------------------------------------------------*/
typedef struct {
	double	A, B, C;
	double	m;
} LINE;


/*---------------------------------------------------------------------
 *	BOX	- 	Specified by two corner points, which are
 *			 sorted to save calculation time later.
 *-------------------------------------------------------------------*/
typedef struct {
	double	xh, yh, xl, yl;		/* high and low coords */
} BOX;

/*---------------------------------------------------------------------
 *  POLYGON - Specified by an array of doubles defining the points, 
 *			  keeping the number of points and the bounding box for 
 *			  speed purposes.
 *-------------------------------------------------------------------*/
typedef struct {
	int32 size;	/* XXX varlena */
	int32 npts;
	BOX boundbox;
	char pts[1];
} POLYGON;


/* 
 * in geo-ops.h
 */
extern BOX *box_in(char *str);
extern char *box_out(BOX *box);
extern BOX *box_construct(double x1, double x2, double y1, double y2);
extern BOX *box_fill(BOX *result, double x1, double x2, double y1, double y2);
extern BOX *box_copy(BOX *box);
extern long box_same(BOX *box1, BOX *box2);
extern long box_overlap(BOX *box1, BOX *box2);
extern long box_overleft(BOX *box1, BOX *box2);
extern long box_left(BOX *box1, BOX *box2);
extern long box_right(BOX *box1, BOX *box2);
extern long box_overright(BOX *box1, BOX *box2);
extern long box_contained(BOX *box1, BOX *box2);
extern long box_contain(BOX *box1, BOX *box2);
extern long box_below(BOX *box1, BOX *box2);
extern long box_above(BOX *box1, BOX *box2);
extern long box_lt(BOX *box1, BOX *box2);
extern long box_gt(BOX *box1, BOX *box2);
extern long box_eq(BOX *box1, BOX *box2);
extern long box_le(BOX *box1, BOX *box2);
extern long box_ge(BOX *box1, BOX *box2);
extern double *box_area(BOX *box);
extern double *box_length(BOX *box);
extern double *box_height(BOX *box);
extern double *box_distance(BOX *box1, BOX *box2);
extern Point *box_center(BOX *box);
extern double box_ar(BOX *box);
extern double box_ln(BOX *box);
extern double box_ht(BOX *box);
extern double box_dt(BOX *box1, BOX *box2);
extern BOX *box_intersect(BOX *box1, BOX *box2);
extern LSEG *box_diagonal(BOX *box);
extern LINE *line_construct_pm(Point *pt, double m);
extern LINE *line_construct_pp(Point *pt1, Point *pt2);
extern long line_intersect(LINE *l1, LINE *l2);
extern long line_parallel(LINE *l1, LINE *l2);
extern long line_perp(LINE *l1, LINE *l2);
extern long line_vertical(LINE *line);
extern long line_horizontal(LINE *line);
extern long line_eq(LINE *l1, LINE *l2);
extern double *line_distance(LINE *l1, LINE *l2);
extern Point *line_interpt(LINE *l1, LINE *l2);
extern PATH *path_in(char *str);
extern char *path_out(PATH *path);
extern long path_n_lt(PATH *p1, PATH *p2);
extern long path_n_gt(PATH *p1, PATH *p2);
extern long path_n_eq(PATH *p1, PATH *p2);
extern long path_n_le(PATH *p1, PATH *p2);
extern long path_n_ge(PATH *p1, PATH *p2);
extern long path_inter(PATH *p1, PATH *p2);
extern double *path_distance(PATH *p1, PATH *p2);
extern double *path_length(PATH *path);
extern double path_ln(PATH *path);
extern Point *point_in(char *str);
extern char *point_out(Point *pt);
extern Point *point_construct(double x, double y);
extern Point *point_copy(Point *pt);
extern long point_left(Point *pt1, Point *pt2);
extern long point_right(Point *pt1, Point *pt2);
extern long point_above(Point *pt1, Point *pt2);
extern long point_below(Point *pt1, Point *pt2);
extern long point_vert(Point *pt1, Point *pt2);
extern long point_horiz(Point *pt1, Point *pt2);
extern long point_eq(Point *pt1, Point *pt2);
extern long pointdist(Point *p1, Point *p2);
extern double *point_distance(Point *pt1, Point *pt2);
extern double point_dt(Point *pt1, Point *pt2);
extern double *point_slope(Point *pt1, Point *pt2);
extern double point_sl(Point *pt1, Point *pt2);
extern LSEG *lseg_in(char *str);
extern char *lseg_out(LSEG *ls);
extern LSEG *lseg_construct(Point *pt1, Point *pt2);
extern void statlseg_construct(LSEG *lseg, Point *pt1, Point *pt2);
extern long lseg_intersect(LSEG *l1, LSEG *l2);
extern long lseg_parallel(LSEG *l1, LSEG *l2);
extern long lseg_perp(LSEG *l1, LSEG *l2);
extern long lseg_vertical(LSEG *lseg);
extern long lseg_horizontal(LSEG *lseg);
extern long lseg_eq(LSEG *l1, LSEG *l2);
extern double *lseg_distance(LSEG *l1, LSEG *l2);
extern double lseg_dt(LSEG *l1, LSEG *l2);
extern Point *lseg_interpt(LSEG *l1, LSEG *l2);
extern double *dist_pl(Point *pt, LINE *line);
extern double *dist_ps(Point *pt, LSEG *lseg);
extern double *dist_ppth(Point *pt, PATH *path);
extern double *dist_pb(Point *pt, BOX *box);
extern double *dist_sl(LSEG *lseg, LINE *line);
extern double *dist_sb(LSEG *lseg, BOX *box);
extern double *dist_lb(LINE *line, BOX *box);
extern Point *interpt_sl(LSEG *lseg, LINE *line);
extern Point *close_pl(Point *pt, LINE *line);
extern Point *close_ps(Point *pt, LSEG *lseg);
extern Point *close_pb(Point *pt, BOX *box);
extern Point *close_sl(LSEG *lseg, LINE *line);
extern Point *close_sb(LSEG *lseg, BOX *box);
extern Point *close_lb(LINE *line, BOX *box);
extern long on_pl(Point *pt, LINE *line);
extern long on_ps(Point *pt, LSEG *lseg);
extern long on_pb(Point *pt, BOX *box);
extern long on_ppath(Point *pt, PATH *path);
extern long on_sl(LSEG *lseg, LINE *line);
extern long on_sb(LSEG *lseg, BOX *box);
extern long inter_sl(LSEG *lseg, LINE *line);
extern long inter_sb(LSEG *lseg, BOX *box);
extern long inter_lb(LINE *line, BOX *box);
extern void make_bound_box(POLYGON *poly);
extern POLYGON *poly_in(char *s);
extern long poly_pt_count(char *s, char delim);
extern char *poly_out(POLYGON *poly);
extern double poly_max(double *coords, int ncoords);
extern double poly_min(double *coords, int ncoords);
extern long poly_left(POLYGON *polya, POLYGON *polyb);
extern long poly_overleft(POLYGON *polya, POLYGON *polyb);
extern long poly_right(POLYGON *polya, POLYGON *polyb);
extern long poly_overright(POLYGON *polya, POLYGON *polyb);
extern long poly_same(POLYGON *polya, POLYGON *polyb);
extern long poly_overlap(POLYGON *polya, POLYGON *polyb);
extern long poly_contain(POLYGON *polya, POLYGON *polyb);
extern long poly_contained(POLYGON *polya, POLYGON *polyb);

/* geo-selfuncs.c */
#if 0	/* FIX ME! */
extern float64 areasel(Oid opid, Oid relid, AttrNumber attno, char *value, int32 flag);
extern float64 areajoinsel(Oid opid, Oid relid, AttrNumber attno, char *value, int32 flag);
extern float64 leftsel(Oid opid, Oid relid, AttrNumber attno, char *value, int32 flag);
extern float64 leftjoinsel(Oid opid, Oid relid, AttrNumber attno, char *value, int32 flag);
extern float64 contsel(Oid opid, Oid relid, AttrNumber attno, char *value, int32 flag);
extern float64 contjoinsel(Oid opid, Oid relid, AttrNumber attno, char *value, int32 flag);
#endif

#endif	/* GEO_DECLS_H */
