#include "pgtz.h"
/*
** This file is in the public domain, so clarified as of
** June 5, 1996 by Arthur David Olson (arthur_david_olson@nih.gov).
*/

#ifndef lint
#ifndef NOID
static char	elsieid[] = "@(#)difftime.c	7.9";
#endif /* !defined NOID */
#endif /* !defined lint */

/*LINTLIBRARY*/

#include "private.h"

/*
** Algorithm courtesy Paul Eggert (eggert@twinsun.com).
*/

#ifdef HAVE_LONG_DOUBLE
#define long_double	long double
#endif /* defined HAVE_LONG_DOUBLE */
#ifndef HAVE_LONG_DOUBLE
#define long_double	double
#endif /* !defined HAVE_LONG_DOUBLE */

double
difftime(time1, time0)
const time_t	time1;
const time_t	time0;
{
	time_t	delta;
	time_t	hibit;

	{
		time_t		tt;
		double		d;
		long_double	ld;

		if (sizeof tt < sizeof d)
			return (double) time1 - (double) time0;
		if (sizeof tt < sizeof ld)
			return (long_double) time1 - (long_double) time0;
	}
	if (time1 < time0)
		return -difftime(time0, time1);
	/*
	** As much as possible, avoid loss of precision
	** by computing the difference before converting to double.
	*/
	delta = time1 - time0;
	if (delta >= 0)
		return delta;
	/*
	** Repair delta overflow.
	*/
	hibit = (~ (time_t) 0) << (TYPE_BIT(time_t) - 1);
	/*
	** The following expression rounds twice, which means
	** the result may not be the closest to the true answer.
	** For example, suppose time_t is 64-bit signed int,
	** long_double is IEEE 754 double with default rounding,
	** time1 = 9223372036854775807 and time0 = -1536.
	** Then the true difference is 9223372036854777343,
	** which rounds to 9223372036854777856
	** with a total error of 513.
	** But delta overflows to -9223372036854774273,
	** which rounds to -9223372036854774784, and correcting
	** this by subtracting 2 * (long_double) hibit
	** (i.e. by adding 2**64 = 18446744073709551616)
	** yields 9223372036854776832, which
	** rounds to 9223372036854775808
	** with a total error of 1535 instead.
	** This problem occurs only with very large differences.
	** It's too painful to fix this portably.
	** We are not alone in this problem;
	** some C compilers round twice when converting
	** large unsigned types to small floating types,
	** so if time_t is unsigned the "return delta" above
	** has the same double-rounding problem with those compilers.
	*/
	return delta - 2 * (long_double) hibit;
}
