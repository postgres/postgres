/*-------------------------------------------------------------------------
 *
 * rint.c
 *	  rint() implementation
 *
 * By Pedro Gimeno Fortea, donated to the public domain
 *
 * IDENTIFICATION
 *	  src/port/rint.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <float.h>
#include <math.h>

/*
 * Round to nearest integer, with halfway cases going to the nearest even.
 */
double
rint(double x)
{
	double		x_orig;
	double		r;

	/* Per POSIX, NaNs must be returned unchanged. */
	if (isnan(x))
		return x;

	if (x <= 0.0)
	{
		/* Both positive and negative zero should be returned unchanged. */
		if (x == 0.0)
			return x;

		/*
		 * Subtracting 0.5 from a number very close to -0.5 can round to
		 * exactly -1.0, producing incorrect results, so we take the opposite
		 * approach: add 0.5 to the negative number, so that it goes closer to
		 * zero (or at most to +0.5, which is dealt with next), avoiding the
		 * precision issue.
		 */
		x_orig = x;
		x += 0.5;

		/*
		 * Be careful to return minus zero when input+0.5 >= 0, as that's what
		 * rint() should return with negative input.
		 */
		if (x >= 0.0)
			return -0.0;

		/*
		 * For very big numbers the input may have no decimals.  That case is
		 * detected by testing x+0.5 == x+1.0; if that happens, the input is
		 * returned unchanged.  This also covers the case of minus infinity.
		 */
		if (x == x_orig + 1.0)
			return x_orig;

		/* Otherwise produce a rounded estimate. */
		r = floor(x);

		/*
		 * If the rounding did not produce exactly input+0.5 then we're done.
		 */
		if (r != x)
			return r;

		/*
		 * The original fractional part was exactly 0.5 (since
		 * floor(input+0.5) == input+0.5).  We need to round to nearest even.
		 * Dividing input+0.5 by 2, taking the floor and multiplying by 2
		 * yields the closest even number.  This part assumes that division by
		 * 2 is exact, which should be OK because underflow is impossible
		 * here: x is an integer.
		 */
		return floor(x * 0.5) * 2.0;
	}
	else
	{
		/*
		 * The positive case is similar but with signs inverted and using
		 * ceil() instead of floor().
		 */
		x_orig = x;
		x -= 0.5;
		if (x <= 0.0)
			return 0.0;
		if (x == x_orig - 1.0)
			return x_orig;
		r = ceil(x);
		if (r != x)
			return r;
		return ceil(x * 0.5) * 2.0;
	}
}
