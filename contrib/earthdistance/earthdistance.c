#include "postgres.h"

#include <math.h>

#include "utils/geo_decls.h"	/* for Pt */


/* Earth's radius is in statute miles. */
const double EARTH_RADIUS = 3958.747716;
const double TWO_PI = 2.0 * M_PI;

double	   *geo_distance(Point *pt1, Point *pt2);


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
 * geo_distance - distance between points
 *
 * args:
 *	 a pair of points - for each point,
 *	   x-coordinate is longitude in degrees west of Greenwich
 *	   y-coordinate is latitude in degrees above equator
 *
 * returns: double
 *	 distance between the points in miles on earth's surface
 ******************************************************/

double *
geo_distance(Point *pt1, Point *pt2)
{

	double		long1,
				lat1,
				long2,
				lat2;
	double		longdiff;
	double		sino;
	double	   *resultp = palloc(sizeof(double));

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
	*resultp = 2. * EARTH_RADIUS * asin(sino);

	return resultp;
}
