/*-------------------------------------------------------------------------
 *
 * geo_decls.h - Declarations for various 2D constructs.
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/geo_decls.h
 *
 * NOTE
 *	  These routines do *not* use the float types from adt/.
 *
 *	  XXX These routines were not written by a numerical analyst.
 *
 *	  XXX I have made some attempt to flesh out the operators
 *		and data types. There are still some more to do. - tgl 97/04/19
 *
 *-------------------------------------------------------------------------
 */
#ifndef GEO_DECLS_H
#define GEO_DECLS_H

#include <math.h>

#include "fmgr.h"

/*--------------------------------------------------------------------
 * Useful floating point utilities and constants.
 *-------------------------------------------------------------------*/


#define EPSILON					1.0E-06

#ifdef EPSILON
#define FPzero(A)				(fabs(A) <= EPSILON)
#define FPeq(A,B)				(fabs((A) - (B)) <= EPSILON)
#define FPne(A,B)				(fabs((A) - (B)) > EPSILON)
#define FPlt(A,B)				((B) - (A) > EPSILON)
#define FPle(A,B)				((A) - (B) <= EPSILON)
#define FPgt(A,B)				((A) - (B) > EPSILON)
#define FPge(A,B)				((B) - (A) <= EPSILON)
#else
#define FPzero(A)				((A) == 0)
#define FPeq(A,B)				((A) == (B))
#define FPne(A,B)				((A) != (B))
#define FPlt(A,B)				((A) < (B))
#define FPle(A,B)				((A) <= (B))
#define FPgt(A,B)				((A) > (B))
#define FPge(A,B)				((A) >= (B))
#endif

#define HYPOT(A, B)				pg_hypot(A, B)

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
} LSEG;


/*---------------------------------------------------------------------
 * PATH - Specified by vertex points.
 *-------------------------------------------------------------------*/
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		npts;
	int32		closed;			/* is this a closed polygon? */
	int32		dummy;			/* padding to make it double align */
	Point		p[FLEXIBLE_ARRAY_MEMBER];
} PATH;


/*---------------------------------------------------------------------
 * LINE - Specified by its general equation (Ax+By+C=0).
 *-------------------------------------------------------------------*/
typedef struct
{
	double		A,
				B,
				C;
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
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		npts;
	BOX			boundbox;
	Point		p[FLEXIBLE_ARRAY_MEMBER];
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
 * fmgr interface macros
 *
 * Path and Polygon are toastable varlena types, the others are just
 * fixed-size pass-by-reference types.
 */

#define DatumGetPointP(X)	 ((Point *) DatumGetPointer(X))
#define PointPGetDatum(X)	 PointerGetDatum(X)
#define PG_GETARG_POINT_P(n) DatumGetPointP(PG_GETARG_DATUM(n))
#define PG_RETURN_POINT_P(x) return PointPGetDatum(x)

#define DatumGetLsegP(X)	((LSEG *) DatumGetPointer(X))
#define LsegPGetDatum(X)	PointerGetDatum(X)
#define PG_GETARG_LSEG_P(n) DatumGetLsegP(PG_GETARG_DATUM(n))
#define PG_RETURN_LSEG_P(x) return LsegPGetDatum(x)

#define DatumGetPathP(X)		 ((PATH *) PG_DETOAST_DATUM(X))
#define DatumGetPathPCopy(X)	 ((PATH *) PG_DETOAST_DATUM_COPY(X))
#define PathPGetDatum(X)		 PointerGetDatum(X)
#define PG_GETARG_PATH_P(n)		 DatumGetPathP(PG_GETARG_DATUM(n))
#define PG_GETARG_PATH_P_COPY(n) DatumGetPathPCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_PATH_P(x)		 return PathPGetDatum(x)

#define DatumGetLineP(X)	((LINE *) DatumGetPointer(X))
#define LinePGetDatum(X)	PointerGetDatum(X)
#define PG_GETARG_LINE_P(n) DatumGetLineP(PG_GETARG_DATUM(n))
#define PG_RETURN_LINE_P(x) return LinePGetDatum(x)

#define DatumGetBoxP(X)    ((BOX *) DatumGetPointer(X))
#define BoxPGetDatum(X)    PointerGetDatum(X)
#define PG_GETARG_BOX_P(n) DatumGetBoxP(PG_GETARG_DATUM(n))
#define PG_RETURN_BOX_P(x) return BoxPGetDatum(x)

#define DatumGetPolygonP(X)			((POLYGON *) PG_DETOAST_DATUM(X))
#define DatumGetPolygonPCopy(X)		((POLYGON *) PG_DETOAST_DATUM_COPY(X))
#define PolygonPGetDatum(X)			PointerGetDatum(X)
#define PG_GETARG_POLYGON_P(n)		DatumGetPolygonP(PG_GETARG_DATUM(n))
#define PG_GETARG_POLYGON_P_COPY(n) DatumGetPolygonPCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_POLYGON_P(x)		return PolygonPGetDatum(x)

#define DatumGetCircleP(X)	  ((CIRCLE *) DatumGetPointer(X))
#define CirclePGetDatum(X)	  PointerGetDatum(X)
#define PG_GETARG_CIRCLE_P(n) DatumGetCircleP(PG_GETARG_DATUM(n))
#define PG_RETURN_CIRCLE_P(x) return CirclePGetDatum(x)


/*
 * in geo_ops.h
 */

/* public point routines */
extern Datum point_in(PG_FUNCTION_ARGS);
extern Datum point_out(PG_FUNCTION_ARGS);
extern Datum point_recv(PG_FUNCTION_ARGS);
extern Datum point_send(PG_FUNCTION_ARGS);
extern Datum construct_point(PG_FUNCTION_ARGS);
extern Datum point_left(PG_FUNCTION_ARGS);
extern Datum point_right(PG_FUNCTION_ARGS);
extern Datum point_above(PG_FUNCTION_ARGS);
extern Datum point_below(PG_FUNCTION_ARGS);
extern Datum point_vert(PG_FUNCTION_ARGS);
extern Datum point_horiz(PG_FUNCTION_ARGS);
extern Datum point_eq(PG_FUNCTION_ARGS);
extern Datum point_ne(PG_FUNCTION_ARGS);
extern Datum point_distance(PG_FUNCTION_ARGS);
extern Datum point_slope(PG_FUNCTION_ARGS);
extern Datum point_add(PG_FUNCTION_ARGS);
extern Datum point_sub(PG_FUNCTION_ARGS);
extern Datum point_mul(PG_FUNCTION_ARGS);
extern Datum point_div(PG_FUNCTION_ARGS);

/* private routines */
extern double point_dt(Point *pt1, Point *pt2);
extern double point_sl(Point *pt1, Point *pt2);
extern double pg_hypot(double x, double y);

/* public lseg routines */
extern Datum lseg_in(PG_FUNCTION_ARGS);
extern Datum lseg_out(PG_FUNCTION_ARGS);
extern Datum lseg_recv(PG_FUNCTION_ARGS);
extern Datum lseg_send(PG_FUNCTION_ARGS);
extern Datum lseg_intersect(PG_FUNCTION_ARGS);
extern Datum lseg_parallel(PG_FUNCTION_ARGS);
extern Datum lseg_perp(PG_FUNCTION_ARGS);
extern Datum lseg_vertical(PG_FUNCTION_ARGS);
extern Datum lseg_horizontal(PG_FUNCTION_ARGS);
extern Datum lseg_eq(PG_FUNCTION_ARGS);
extern Datum lseg_ne(PG_FUNCTION_ARGS);
extern Datum lseg_lt(PG_FUNCTION_ARGS);
extern Datum lseg_le(PG_FUNCTION_ARGS);
extern Datum lseg_gt(PG_FUNCTION_ARGS);
extern Datum lseg_ge(PG_FUNCTION_ARGS);
extern Datum lseg_construct(PG_FUNCTION_ARGS);
extern Datum lseg_length(PG_FUNCTION_ARGS);
extern Datum lseg_distance(PG_FUNCTION_ARGS);
extern Datum lseg_center(PG_FUNCTION_ARGS);
extern Datum lseg_interpt(PG_FUNCTION_ARGS);
extern Datum dist_pl(PG_FUNCTION_ARGS);
extern Datum dist_ps(PG_FUNCTION_ARGS);
extern Datum dist_ppath(PG_FUNCTION_ARGS);
extern Datum dist_pb(PG_FUNCTION_ARGS);
extern Datum dist_sl(PG_FUNCTION_ARGS);
extern Datum dist_sb(PG_FUNCTION_ARGS);
extern Datum dist_lb(PG_FUNCTION_ARGS);
extern Datum close_lseg(PG_FUNCTION_ARGS);
extern Datum close_pl(PG_FUNCTION_ARGS);
extern Datum close_ps(PG_FUNCTION_ARGS);
extern Datum close_pb(PG_FUNCTION_ARGS);
extern Datum close_sl(PG_FUNCTION_ARGS);
extern Datum close_sb(PG_FUNCTION_ARGS);
extern Datum close_ls(PG_FUNCTION_ARGS);
extern Datum close_lb(PG_FUNCTION_ARGS);
extern Datum on_pl(PG_FUNCTION_ARGS);
extern Datum on_ps(PG_FUNCTION_ARGS);
extern Datum on_pb(PG_FUNCTION_ARGS);
extern Datum on_ppath(PG_FUNCTION_ARGS);
extern Datum on_sl(PG_FUNCTION_ARGS);
extern Datum on_sb(PG_FUNCTION_ARGS);
extern Datum inter_sl(PG_FUNCTION_ARGS);
extern Datum inter_sb(PG_FUNCTION_ARGS);
extern Datum inter_lb(PG_FUNCTION_ARGS);

/* public line routines */
extern Datum line_in(PG_FUNCTION_ARGS);
extern Datum line_out(PG_FUNCTION_ARGS);
extern Datum line_recv(PG_FUNCTION_ARGS);
extern Datum line_send(PG_FUNCTION_ARGS);
extern Datum line_interpt(PG_FUNCTION_ARGS);
extern Datum line_distance(PG_FUNCTION_ARGS);
extern Datum line_construct_pp(PG_FUNCTION_ARGS);
extern Datum line_intersect(PG_FUNCTION_ARGS);
extern Datum line_parallel(PG_FUNCTION_ARGS);
extern Datum line_perp(PG_FUNCTION_ARGS);
extern Datum line_vertical(PG_FUNCTION_ARGS);
extern Datum line_horizontal(PG_FUNCTION_ARGS);
extern Datum line_eq(PG_FUNCTION_ARGS);

/* public box routines */
extern Datum box_in(PG_FUNCTION_ARGS);
extern Datum box_out(PG_FUNCTION_ARGS);
extern Datum box_recv(PG_FUNCTION_ARGS);
extern Datum box_send(PG_FUNCTION_ARGS);
extern Datum box_same(PG_FUNCTION_ARGS);
extern Datum box_overlap(PG_FUNCTION_ARGS);
extern Datum box_left(PG_FUNCTION_ARGS);
extern Datum box_overleft(PG_FUNCTION_ARGS);
extern Datum box_right(PG_FUNCTION_ARGS);
extern Datum box_overright(PG_FUNCTION_ARGS);
extern Datum box_below(PG_FUNCTION_ARGS);
extern Datum box_overbelow(PG_FUNCTION_ARGS);
extern Datum box_above(PG_FUNCTION_ARGS);
extern Datum box_overabove(PG_FUNCTION_ARGS);
extern Datum box_contained(PG_FUNCTION_ARGS);
extern Datum box_contain(PG_FUNCTION_ARGS);
extern Datum box_contain_pt(PG_FUNCTION_ARGS);
extern Datum box_below_eq(PG_FUNCTION_ARGS);
extern Datum box_above_eq(PG_FUNCTION_ARGS);
extern Datum box_lt(PG_FUNCTION_ARGS);
extern Datum box_gt(PG_FUNCTION_ARGS);
extern Datum box_eq(PG_FUNCTION_ARGS);
extern Datum box_le(PG_FUNCTION_ARGS);
extern Datum box_ge(PG_FUNCTION_ARGS);
extern Datum box_area(PG_FUNCTION_ARGS);
extern Datum box_width(PG_FUNCTION_ARGS);
extern Datum box_height(PG_FUNCTION_ARGS);
extern Datum box_distance(PG_FUNCTION_ARGS);
extern Datum box_center(PG_FUNCTION_ARGS);
extern Datum box_intersect(PG_FUNCTION_ARGS);
extern Datum box_diagonal(PG_FUNCTION_ARGS);
extern Datum points_box(PG_FUNCTION_ARGS);
extern Datum box_add(PG_FUNCTION_ARGS);
extern Datum box_sub(PG_FUNCTION_ARGS);
extern Datum box_mul(PG_FUNCTION_ARGS);
extern Datum box_div(PG_FUNCTION_ARGS);
extern Datum point_box(PG_FUNCTION_ARGS);
extern Datum boxes_bound_box(PG_FUNCTION_ARGS);

/* public path routines */
extern Datum path_area(PG_FUNCTION_ARGS);
extern Datum path_in(PG_FUNCTION_ARGS);
extern Datum path_out(PG_FUNCTION_ARGS);
extern Datum path_recv(PG_FUNCTION_ARGS);
extern Datum path_send(PG_FUNCTION_ARGS);
extern Datum path_n_lt(PG_FUNCTION_ARGS);
extern Datum path_n_gt(PG_FUNCTION_ARGS);
extern Datum path_n_eq(PG_FUNCTION_ARGS);
extern Datum path_n_le(PG_FUNCTION_ARGS);
extern Datum path_n_ge(PG_FUNCTION_ARGS);
extern Datum path_inter(PG_FUNCTION_ARGS);
extern Datum path_distance(PG_FUNCTION_ARGS);
extern Datum path_length(PG_FUNCTION_ARGS);

extern Datum path_isclosed(PG_FUNCTION_ARGS);
extern Datum path_isopen(PG_FUNCTION_ARGS);
extern Datum path_npoints(PG_FUNCTION_ARGS);

extern Datum path_close(PG_FUNCTION_ARGS);
extern Datum path_open(PG_FUNCTION_ARGS);
extern Datum path_add(PG_FUNCTION_ARGS);
extern Datum path_add_pt(PG_FUNCTION_ARGS);
extern Datum path_sub_pt(PG_FUNCTION_ARGS);
extern Datum path_mul_pt(PG_FUNCTION_ARGS);
extern Datum path_div_pt(PG_FUNCTION_ARGS);

extern Datum path_center(PG_FUNCTION_ARGS);
extern Datum path_poly(PG_FUNCTION_ARGS);

/* public polygon routines */
extern Datum poly_in(PG_FUNCTION_ARGS);
extern Datum poly_out(PG_FUNCTION_ARGS);
extern Datum poly_recv(PG_FUNCTION_ARGS);
extern Datum poly_send(PG_FUNCTION_ARGS);
extern Datum poly_left(PG_FUNCTION_ARGS);
extern Datum poly_overleft(PG_FUNCTION_ARGS);
extern Datum poly_right(PG_FUNCTION_ARGS);
extern Datum poly_overright(PG_FUNCTION_ARGS);
extern Datum poly_below(PG_FUNCTION_ARGS);
extern Datum poly_overbelow(PG_FUNCTION_ARGS);
extern Datum poly_above(PG_FUNCTION_ARGS);
extern Datum poly_overabove(PG_FUNCTION_ARGS);
extern Datum poly_same(PG_FUNCTION_ARGS);
extern Datum poly_overlap(PG_FUNCTION_ARGS);
extern Datum poly_contain(PG_FUNCTION_ARGS);
extern Datum poly_contained(PG_FUNCTION_ARGS);
extern Datum poly_contain_pt(PG_FUNCTION_ARGS);
extern Datum pt_contained_poly(PG_FUNCTION_ARGS);
extern Datum poly_distance(PG_FUNCTION_ARGS);
extern Datum poly_npoints(PG_FUNCTION_ARGS);
extern Datum poly_center(PG_FUNCTION_ARGS);
extern Datum poly_box(PG_FUNCTION_ARGS);
extern Datum poly_path(PG_FUNCTION_ARGS);
extern Datum box_poly(PG_FUNCTION_ARGS);

/* public circle routines */
extern Datum circle_in(PG_FUNCTION_ARGS);
extern Datum circle_out(PG_FUNCTION_ARGS);
extern Datum circle_recv(PG_FUNCTION_ARGS);
extern Datum circle_send(PG_FUNCTION_ARGS);
extern Datum circle_same(PG_FUNCTION_ARGS);
extern Datum circle_overlap(PG_FUNCTION_ARGS);
extern Datum circle_overleft(PG_FUNCTION_ARGS);
extern Datum circle_left(PG_FUNCTION_ARGS);
extern Datum circle_right(PG_FUNCTION_ARGS);
extern Datum circle_overright(PG_FUNCTION_ARGS);
extern Datum circle_contained(PG_FUNCTION_ARGS);
extern Datum circle_contain(PG_FUNCTION_ARGS);
extern Datum circle_below(PG_FUNCTION_ARGS);
extern Datum circle_above(PG_FUNCTION_ARGS);
extern Datum circle_overbelow(PG_FUNCTION_ARGS);
extern Datum circle_overabove(PG_FUNCTION_ARGS);
extern Datum circle_eq(PG_FUNCTION_ARGS);
extern Datum circle_ne(PG_FUNCTION_ARGS);
extern Datum circle_lt(PG_FUNCTION_ARGS);
extern Datum circle_gt(PG_FUNCTION_ARGS);
extern Datum circle_le(PG_FUNCTION_ARGS);
extern Datum circle_ge(PG_FUNCTION_ARGS);
extern Datum circle_contain_pt(PG_FUNCTION_ARGS);
extern Datum pt_contained_circle(PG_FUNCTION_ARGS);
extern Datum circle_add_pt(PG_FUNCTION_ARGS);
extern Datum circle_sub_pt(PG_FUNCTION_ARGS);
extern Datum circle_mul_pt(PG_FUNCTION_ARGS);
extern Datum circle_div_pt(PG_FUNCTION_ARGS);
extern Datum circle_diameter(PG_FUNCTION_ARGS);
extern Datum circle_radius(PG_FUNCTION_ARGS);
extern Datum circle_distance(PG_FUNCTION_ARGS);
extern Datum dist_pc(PG_FUNCTION_ARGS);
extern Datum dist_cpoint(PG_FUNCTION_ARGS);
extern Datum dist_cpoly(PG_FUNCTION_ARGS);
extern Datum dist_ppoly(PG_FUNCTION_ARGS);
extern Datum dist_polyp(PG_FUNCTION_ARGS);
extern Datum circle_center(PG_FUNCTION_ARGS);
extern Datum cr_circle(PG_FUNCTION_ARGS);
extern Datum box_circle(PG_FUNCTION_ARGS);
extern Datum circle_box(PG_FUNCTION_ARGS);
extern Datum poly_circle(PG_FUNCTION_ARGS);
extern Datum circle_poly(PG_FUNCTION_ARGS);
extern Datum circle_area(PG_FUNCTION_ARGS);

/* support routines for the GiST access method (access/gist/gistproc.c) */
extern Datum gist_box_compress(PG_FUNCTION_ARGS);
extern Datum gist_box_decompress(PG_FUNCTION_ARGS);
extern Datum gist_box_union(PG_FUNCTION_ARGS);
extern Datum gist_box_picksplit(PG_FUNCTION_ARGS);
extern Datum gist_box_consistent(PG_FUNCTION_ARGS);
extern Datum gist_box_penalty(PG_FUNCTION_ARGS);
extern Datum gist_box_same(PG_FUNCTION_ARGS);
extern Datum gist_box_fetch(PG_FUNCTION_ARGS);
extern Datum gist_poly_compress(PG_FUNCTION_ARGS);
extern Datum gist_poly_consistent(PG_FUNCTION_ARGS);
extern Datum gist_circle_compress(PG_FUNCTION_ARGS);
extern Datum gist_circle_consistent(PG_FUNCTION_ARGS);
extern Datum gist_point_compress(PG_FUNCTION_ARGS);
extern Datum gist_point_consistent(PG_FUNCTION_ARGS);
extern Datum gist_point_distance(PG_FUNCTION_ARGS);
extern Datum gist_bbox_distance(PG_FUNCTION_ARGS);
extern Datum gist_point_fetch(PG_FUNCTION_ARGS);


/* geo_selfuncs.c */
extern Datum areasel(PG_FUNCTION_ARGS);
extern Datum areajoinsel(PG_FUNCTION_ARGS);
extern Datum positionsel(PG_FUNCTION_ARGS);
extern Datum positionjoinsel(PG_FUNCTION_ARGS);
extern Datum contsel(PG_FUNCTION_ARGS);
extern Datum contjoinsel(PG_FUNCTION_ARGS);

#endif   /* GEO_DECLS_H */
