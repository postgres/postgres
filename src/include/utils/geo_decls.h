/*-------------------------------------------------------------------------
 *
 * geo_decls.h - Declarations for various 2D constructs.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/geo_decls.h
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
 *--------------------------------------------------------------------
 *
 * "Fuzzy" floating-point comparisons: values within EPSILON of each other
 * are considered equal.  Beware of normal reasoning about the behavior of
 * these comparisons, since for example FPeq does not behave transitively.
 *
 * Note that these functions are not NaN-aware and will give FALSE for
 * any case involving NaN inputs.
 *
 * Also note that these will give sane answers for infinite inputs,
 * where it's important to avoid computing Inf minus Inf; we do so
 * by eliminating equality cases before subtracting.
 */

#define EPSILON					1.0E-06

#ifdef EPSILON
#define FPzero(A)				(fabs(A) <= EPSILON)

static inline bool
FPeq(double A, double B)
{
	return A == B || fabs(A - B) <= EPSILON;
}

static inline bool
FPne(double A, double B)
{
	return A != B && fabs(A - B) > EPSILON;
}

static inline bool
FPlt(double A, double B)
{
	return A + EPSILON < B;
}

static inline bool
FPle(double A, double B)
{
	return A <= B + EPSILON;
}

static inline bool
FPgt(double A, double B)
{
	return A > B + EPSILON;
}

static inline bool
FPge(double A, double B)
{
	return A + EPSILON >= B;
}
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
	float8		x,
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
	float8		A,
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
	float8		radius;
} CIRCLE;

/*
 * fmgr interface functions
 *
 * Path and Polygon are toastable varlena types, the others are just
 * fixed-size pass-by-reference types.
 */

static inline Point *
DatumGetPointP(Datum X)
{
	return (Point *) DatumGetPointer(X);
}
static inline Datum
PointPGetDatum(const Point *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_POINT_P(n) DatumGetPointP(PG_GETARG_DATUM(n))
#define PG_RETURN_POINT_P(x) return PointPGetDatum(x)

static inline LSEG *
DatumGetLsegP(Datum X)
{
	return (LSEG *) DatumGetPointer(X);
}
static inline Datum
LsegPGetDatum(const LSEG *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_LSEG_P(n) DatumGetLsegP(PG_GETARG_DATUM(n))
#define PG_RETURN_LSEG_P(x) return LsegPGetDatum(x)

static inline PATH *
DatumGetPathP(Datum X)
{
	return (PATH *) PG_DETOAST_DATUM(X);
}
static inline PATH *
DatumGetPathPCopy(Datum X)
{
	return (PATH *) PG_DETOAST_DATUM_COPY(X);
}
static inline Datum
PathPGetDatum(const PATH *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_PATH_P(n)		 DatumGetPathP(PG_GETARG_DATUM(n))
#define PG_GETARG_PATH_P_COPY(n) DatumGetPathPCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_PATH_P(x)		 return PathPGetDatum(x)

static inline LINE *
DatumGetLineP(Datum X)
{
	return (LINE *) DatumGetPointer(X);
}
static inline Datum
LinePGetDatum(const LINE *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_LINE_P(n) DatumGetLineP(PG_GETARG_DATUM(n))
#define PG_RETURN_LINE_P(x) return LinePGetDatum(x)

static inline BOX *
DatumGetBoxP(Datum X)
{
	return (BOX *) DatumGetPointer(X);
}
static inline Datum
BoxPGetDatum(const BOX *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_BOX_P(n) DatumGetBoxP(PG_GETARG_DATUM(n))
#define PG_RETURN_BOX_P(x) return BoxPGetDatum(x)

static inline POLYGON *
DatumGetPolygonP(Datum X)
{
	return (POLYGON *) PG_DETOAST_DATUM(X);
}
static inline POLYGON *
DatumGetPolygonPCopy(Datum X)
{
	return (POLYGON *) PG_DETOAST_DATUM_COPY(X);
}
static inline Datum
PolygonPGetDatum(const POLYGON *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_POLYGON_P(n)		DatumGetPolygonP(PG_GETARG_DATUM(n))
#define PG_GETARG_POLYGON_P_COPY(n) DatumGetPolygonPCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_POLYGON_P(x)		return PolygonPGetDatum(x)

static inline CIRCLE *
DatumGetCircleP(Datum X)
{
	return (CIRCLE *) DatumGetPointer(X);
}
static inline Datum
CirclePGetDatum(const CIRCLE *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_CIRCLE_P(n) DatumGetCircleP(PG_GETARG_DATUM(n))
#define PG_RETURN_CIRCLE_P(x) return CirclePGetDatum(x)


/*
 * in geo_ops.c
 */

extern float8 pg_hypot(float8 x, float8 y);

#endif							/* GEO_DECLS_H */
