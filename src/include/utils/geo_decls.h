/*-------------------------------------------------------------------------
 *
 * geo_decls.h - Declarations for various 2D constructs.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geo_decls.h,v 1.19 1998/02/26 04:44:00 momjian Exp $
 *
 * NOTE
 *	  These routines do *not* use the float types from adt/.
 *
 *	  XXX These routines were not written by a numerical analyst.
 *	  XXX I have made some attempt to flesh out the operators
 *		and data types. There are still some more to do. - tgl 97/04/19
 *
 *-------------------------------------------------------------------------
 */
#ifndef GEO_DECLS_H
#define GEO_DECLS_H

#include "access/attnum.h"

/*--------------------------------------------------------------------
 * Useful floating point utilities and constants.
 *-------------------------------------------------------------------*/


#define EPSILON					1.0E-06

#ifdef EPSILON
#define FPzero(A)				(fabs(A) <= EPSILON)
#define FPeq(A,B)				(fabs((A) - (B)) <= EPSILON)
#define FPlt(A,B)				((B) - (A) > EPSILON)
#define FPle(A,B)				((A) - (B) <= EPSILON)
#define FPgt(A,B)				((A) - (B) > EPSILON)
#define FPge(A,B)				((B) - (A) <= EPSILON)
#else
#define FPzero(A)				(A == 0)
#define FPnzero(A)				(A != 0)
#define FPeq(A,B)				(A == B)
#define FPne(A,B)				(A != B)
#define FPlt(A,B)				(A < B)
#define FPle(A,B)				(A <= B)
#define FPgt(A,B)				(A > B)
#define FPge(A,B)				(A >= B)
#endif

#define HYPOT(A, B)				sqrt((A) * (A) + (B) * (B))

/*---------------------------------------------------------------------
 * Point - (x,y)
 *-------------------------------------------------------------------*/
typedef struct
{
	double		x,
				y;
} Point;


/*---------------------------------------------------------------------
 * LSEG - A straight line, specified by endpoints.
 *-------------------------------------------------------------------*/
typedef struct
{
	Point		p[2];

	double		m;				/* precomputed to save time, not in tuple */
} LSEG;


/*---------------------------------------------------------------------
 * PATH - Specified by vertex points.
 *-------------------------------------------------------------------*/
typedef struct
{
	int32		size;			/* XXX varlena */
	int32		npts;
	int32		closed;			/* is this a closed polygon? */
	int32		dummy;			/* padding to make it double align */
	Point		p[1];			/* variable length array of POINTs */
} PATH;


/*---------------------------------------------------------------------
 * LINE - Specified by its general equation (Ax+By+C=0).
 *		If there is a y-intercept, it is C, which
 *		 incidentally gives a freebie point on the line
 *		 (if B=0, then C is the x-intercept).
 *		Slope m is precalculated to save time; if
 *		 the line is not vertical, m == A.
 *-------------------------------------------------------------------*/
typedef struct
{
	double		A,
				B,
				C;

	double		m;
} LINE;


/*---------------------------------------------------------------------
 * BOX	- Specified by two corner points, which are
 *		 sorted to save calculation time later.
 *-------------------------------------------------------------------*/
typedef struct
{
	Point		high,
				low;			/* corner POINTs */
} BOX;

/*---------------------------------------------------------------------
 * POLYGON - Specified by an array of doubles defining the points,
 *		keeping the number of points and the bounding box for
 *		speed purposes.
 *-------------------------------------------------------------------*/
typedef struct
{
	int32		size;			/* XXX varlena */
	int32		npts;
	BOX			boundbox;
	Point		p[1];			/* variable length array of POINTs */
} POLYGON;

/*---------------------------------------------------------------------
 * CIRCLE - Specified by a center point and radius.
 *-------------------------------------------------------------------*/
typedef struct
{
	Point		center;
	double		radius;
} CIRCLE;

/*
 * in geo_ops.h
 */

/* public point routines */
extern Point *point_in(char *str);
extern char *point_out(Point *pt);
extern bool point_left(Point *pt1, Point *pt2);
extern bool point_right(Point *pt1, Point *pt2);
extern bool point_above(Point *pt1, Point *pt2);
extern bool point_below(Point *pt1, Point *pt2);
extern bool point_vert(Point *pt1, Point *pt2);
extern bool point_horiz(Point *pt1, Point *pt2);
extern bool point_eq(Point *pt1, Point *pt2);
extern bool point_ne(Point *pt1, Point *pt2);
extern int32 pointdist(Point *p1, Point *p2);
extern double *point_distance(Point *pt1, Point *pt2);
extern double *point_slope(Point *pt1, Point *pt2);

/* private routines */
extern double point_dt(Point *pt1, Point *pt2);
extern double point_sl(Point *pt1, Point *pt2);

extern Point *point(float8 *x, float8 *y);
extern Point *point_add(Point *p1, Point *p2);
extern Point *point_sub(Point *p1, Point *p2);
extern Point *point_mul(Point *p1, Point *p2);
extern Point *point_div(Point *p1, Point *p2);

/* public lseg routines */
extern LSEG *lseg_in(char *str);
extern char *lseg_out(LSEG *ls);
extern bool lseg_intersect(LSEG *l1, LSEG *l2);
extern bool lseg_parallel(LSEG *l1, LSEG *l2);
extern bool lseg_perp(LSEG *l1, LSEG *l2);
extern bool lseg_vertical(LSEG *lseg);
extern bool lseg_horizontal(LSEG *lseg);
extern bool lseg_eq(LSEG *l1, LSEG *l2);
extern bool lseg_ne(LSEG *l1, LSEG *l2);
extern bool lseg_lt(LSEG *l1, LSEG *l2);
extern bool lseg_le(LSEG *l1, LSEG *l2);
extern bool lseg_gt(LSEG *l1, LSEG *l2);
extern bool lseg_ge(LSEG *l1, LSEG *l2);
extern double *lseg_length(LSEG *lseg);
extern double *lseg_distance(LSEG *l1, LSEG *l2);
extern Point *lseg_center(LSEG *lseg);
extern Point *lseg_interpt(LSEG *l1, LSEG *l2);
extern double *dist_pl(Point *pt, LINE *line);
extern double *dist_ps(Point *pt, LSEG *lseg);
extern double *dist_ppath(Point *pt, PATH *path);
extern double *dist_pb(Point *pt, BOX *box);
extern double *dist_sl(LSEG *lseg, LINE *line);
extern double *dist_sb(LSEG *lseg, BOX *box);
extern double *dist_lb(LINE *line, BOX *box);
extern Point *close_lseg(LSEG *l1, LSEG *l2);
extern Point *close_pl(Point *pt, LINE *line);
extern Point *close_ps(Point *pt, LSEG *lseg);
extern Point *close_pb(Point *pt, BOX *box);
extern Point *close_sl(LSEG *lseg, LINE *line);
extern Point *close_sb(LSEG *lseg, BOX *box);
extern Point *close_ls(LINE *line, LSEG *lseg);
extern Point *close_lb(LINE *line, BOX *box);
extern bool on_pl(Point *pt, LINE *line);
extern bool on_ps(Point *pt, LSEG *lseg);
extern bool on_pb(Point *pt, BOX *box);
extern bool on_ppath(Point *pt, PATH *path);
extern bool on_sl(LSEG *lseg, LINE *line);
extern bool on_sb(LSEG *lseg, BOX *box);
extern bool inter_sl(LSEG *lseg, LINE *line);
extern bool inter_sb(LSEG *lseg, BOX *box);
extern bool inter_lb(LINE *line, BOX *box);

/* private routines */
extern LSEG *lseg_construct(Point *pt1, Point *pt2);

/* public box routines */
extern BOX *box_in(char *str);
extern char *box_out(BOX *box);
extern bool box_same(BOX *box1, BOX *box2);
extern bool box_overlap(BOX *box1, BOX *box2);
extern bool box_overleft(BOX *box1, BOX *box2);
extern bool box_left(BOX *box1, BOX *box2);
extern bool box_right(BOX *box1, BOX *box2);
extern bool box_overright(BOX *box1, BOX *box2);
extern bool box_contained(BOX *box1, BOX *box2);
extern bool box_contain(BOX *box1, BOX *box2);
extern bool box_below(BOX *box1, BOX *box2);
extern bool box_above(BOX *box1, BOX *box2);
extern bool box_lt(BOX *box1, BOX *box2);
extern bool box_gt(BOX *box1, BOX *box2);
extern bool box_eq(BOX *box1, BOX *box2);
extern bool box_le(BOX *box1, BOX *box2);
extern bool box_ge(BOX *box1, BOX *box2);
extern Point *box_center(BOX *box);
extern double *box_area(BOX *box);
extern double *box_width(BOX *box);
extern double *box_height(BOX *box);
extern double *box_distance(BOX *box1, BOX *box2);
extern Point *box_center(BOX *box);
extern BOX *box_intersect(BOX *box1, BOX *box2);
extern LSEG *box_diagonal(BOX *box);

/* private routines */

extern double box_dt(BOX *box1, BOX *box2);

extern BOX *box(Point *p1, Point *p2);
extern BOX *box_add(BOX *box, Point *p);
extern BOX *box_sub(BOX *box, Point *p);
extern BOX *box_mul(BOX *box, Point *p);
extern BOX *box_div(BOX *box, Point *p);

/* private line routines */
extern double *line_distance(LINE *l1, LINE *l2);

/* public path routines */
extern PATH *path_in(char *str);
extern char *path_out(PATH *path);
extern bool path_n_lt(PATH *p1, PATH *p2);
extern bool path_n_gt(PATH *p1, PATH *p2);
extern bool path_n_eq(PATH *p1, PATH *p2);
extern bool path_n_le(PATH *p1, PATH *p2);
extern bool path_n_ge(PATH *p1, PATH *p2);
extern bool path_inter(PATH *p1, PATH *p2);
extern double *path_distance(PATH *p1, PATH *p2);
extern double *path_length(PATH *path);

extern bool path_isclosed(PATH *path);
extern bool path_isopen(PATH *path);
extern int4 path_npoints(PATH *path);

extern PATH *path_close(PATH *path);
extern PATH *path_open(PATH *path);
extern PATH *path_add(PATH *p1, PATH *p2);
extern PATH *path_add_pt(PATH *path, Point *point);
extern PATH *path_sub_pt(PATH *path, Point *point);
extern PATH *path_mul_pt(PATH *path, Point *point);
extern PATH *path_div_pt(PATH *path, Point *point);
extern bool path_contain_pt(PATH *path, Point *p);
extern bool pt_contained_path(Point *p, PATH *path);

extern Point *path_center(PATH *path);
extern POLYGON *path_poly(PATH *path);

extern PATH *upgradepath(PATH *path);
extern bool isoldpath(PATH *path);

/* public polygon routines */
extern POLYGON *poly_in(char *s);
extern char *poly_out(POLYGON *poly);
extern bool poly_left(POLYGON *polya, POLYGON *polyb);
extern bool poly_overleft(POLYGON *polya, POLYGON *polyb);
extern bool poly_right(POLYGON *polya, POLYGON *polyb);
extern bool poly_overright(POLYGON *polya, POLYGON *polyb);
extern bool poly_same(POLYGON *polya, POLYGON *polyb);
extern bool poly_overlap(POLYGON *polya, POLYGON *polyb);
extern bool poly_contain(POLYGON *polya, POLYGON *polyb);
extern bool poly_contained(POLYGON *polya, POLYGON *polyb);
extern bool poly_contain_pt(POLYGON *poly, Point *p);
extern bool pt_contained_poly(Point *p, POLYGON *poly);

extern double *poly_distance(POLYGON *polya, POLYGON *polyb);
extern int4 poly_npoints(POLYGON *poly);
extern Point *poly_center(POLYGON *poly);
extern BOX *poly_box(POLYGON *poly);
extern PATH *poly_path(POLYGON *poly);
extern POLYGON *box_poly(BOX *box);

extern POLYGON *upgradepoly(POLYGON *poly);
extern POLYGON *revertpoly(POLYGON *poly);

/* private polygon routines */

/* public circle routines */
extern CIRCLE *circle_in(char *str);
extern char *circle_out(CIRCLE *circle);
extern bool circle_same(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_overlap(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_overleft(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_left(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_right(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_overright(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_contained(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_contain(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_below(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_above(CIRCLE *circle1, CIRCLE *circle2);

extern bool circle_eq(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_ne(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_lt(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_gt(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_le(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_ge(CIRCLE *circle1, CIRCLE *circle2);
extern bool circle_contain_pt(CIRCLE *circle, Point *point);
extern bool pt_contained_circle(Point *point, CIRCLE *circle);
extern CIRCLE *circle_add_pt(CIRCLE *circle, Point *point);
extern CIRCLE *circle_sub_pt(CIRCLE *circle, Point *point);
extern CIRCLE *circle_mul_pt(CIRCLE *circle, Point *point);
extern CIRCLE *circle_div_pt(CIRCLE *circle, Point *point);
extern double *circle_diameter(CIRCLE *circle);
extern double *circle_radius(CIRCLE *circle);
extern double *circle_distance(CIRCLE *circle1, CIRCLE *circle2);
extern double *dist_pc(Point *point, CIRCLE *circle);
extern double *dist_cpoly(CIRCLE *circle, POLYGON *poly);
extern Point *circle_center(CIRCLE *circle);
extern CIRCLE *circle(Point *center, float8 *radius);
extern CIRCLE *box_circle(BOX *box);
extern BOX *circle_box(CIRCLE *circle);
extern CIRCLE *poly_circle(POLYGON *poly);
extern POLYGON *circle_poly(int npts, CIRCLE *circle);

/* private routines */
extern double *circle_area(CIRCLE *circle);
extern double circle_dt(CIRCLE *circle1, CIRCLE *circle2);

/* geo_selfuncs.c */
extern float64
areasel(Oid opid, Oid relid, AttrNumber attno,
		char *value, int32 flag);
extern float64
areajoinsel(Oid opid, Oid relid, AttrNumber attno,
			char *value, int32 flag);

#endif							/* GEO_DECLS_H */
