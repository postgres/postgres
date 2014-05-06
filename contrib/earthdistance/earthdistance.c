/* contrib/earthdistance/earthdistance.c */

#include "postgres.h"

#include <math.h>

#include "utils/geo_decls.h"	/* for Point */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


PG_MODULE_MAGIC;

/* Earth's radius is in statute miles. */
static const double EARTH_RADIUS = 3958.747716;
static const double TWO_PI = 2.0 * M_PI;


/******************************************************
 *
 * degtorad - convert degrees to radians
 *
 * arg: double, angle in degrees
 *
 * returns: double, same angle in radians
 ******************************************************/

static double
degtorad(double degrees)
{
	return (degrees / 360.0) * TWO_PI;
}

/******************************************************
 *
 * geo_distance_internal - distance between points
 *
 * args:
 *	 a pair of points - for each point,
 *	   x-coordinate is longitude in degrees west of Greenwich
 *	   y-coordinate is latitude in degrees above equator
 *
 * returns: double
 *	 distance between the points in miles on earth's surface
 ******************************************************/

static double
geo_distance_internal(Point *pt1, Point *pt2)
{
	double		long1,
				lat1,
				long2,
				lat2;
	double		longdiff;
	double		sino;

	/* convert degrees to radians */

	long1 = degtorad(pt1->x);
	lat1 = degtorad(pt1->y);

	long2 = degtorad(pt2->x);
	lat2 = degtorad(pt2->y);

	/* compute difference in longitudes - want < 180 degrees */
	longdiff = fabs(long1 - long2);
	if (longdiff > M_PI)
		longdiff = TWO_PI - longdiff;

	sino = sqrt(sin(fabs(lat1 - lat2) / 2.) * sin(fabs(lat1 - lat2) / 2.) +
			cos(lat1) * cos(lat2) * sin(longdiff / 2.) * sin(longdiff / 2.));
	if (sino > 1.)
		sino = 1.;

	return 2. * EARTH_RADIUS * asin(sino);
}


/******************************************************
 *
 * geo_distance - distance between points
 *
 * args:
 *	 a pair of points - for each point,
 *	   x-coordinate is longitude in degrees west of Greenwich
 *	   y-coordinate is latitude in degrees above equator
 *
 * returns: float8
 *	 distance between the points in miles on earth's surface
 *
 * If float8 is passed-by-value, the oldstyle version-0 calling convention
 * is unportable, so we use version-1.  However, if it's passed-by-reference,
 * continue to use oldstyle.  This is just because we'd like earthdistance
 * to serve as a canary for any unintentional breakage of version-0 functions
 * with float8 results.
 ******************************************************/

#ifdef USE_FLOAT8_BYVAL

PG_FUNCTION_INFO_V1(geo_distance);

Datum
geo_distance(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);
	float8		result;

	result = geo_distance_internal(pt1, pt2);
	PG_RETURN_FLOAT8(result);
}
#else							/* !USE_FLOAT8_BYVAL */

double	   *geo_distance(Point *pt1, Point *pt2);

double *
geo_distance(Point *pt1, Point *pt2)
{
	double	   *resultp = palloc(sizeof(double));

	*resultp = geo_distance_internal(pt1, pt2);
	return resultp;
}

#endif   /* USE_FLOAT8_BYVAL */
