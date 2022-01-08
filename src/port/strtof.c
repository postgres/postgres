/*-------------------------------------------------------------------------
 *
 * strtof.c
 *
 * Portions Copyright (c) 2019-2022, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/port/strtof.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <float.h>
#include <math.h>

#ifndef HAVE_STRTOF
/*
 * strtof() is part of C99; this version is only for the benefit of obsolete
 * platforms. As such, it is known to return incorrect values for edge cases,
 * which have to be allowed for in variant files for regression test results
 * for any such platform.
 */

float
strtof(const char *nptr, char **endptr)
{
	int			caller_errno = errno;
	double		dresult;
	float		fresult;

	errno = 0;
	dresult = strtod(nptr, endptr);
	fresult = (float) dresult;

	if (errno == 0)
	{
		/*
		 * Value might be in-range for double but not float.
		 */
		if (dresult != 0 && fresult == 0)
			caller_errno = ERANGE;	/* underflow */
		if (!isinf(dresult) && isinf(fresult))
			caller_errno = ERANGE;	/* overflow */
	}
	else
		caller_errno = errno;

	errno = caller_errno;
	return fresult;
}

#elif HAVE_BUGGY_STRTOF
/*
 * On Windows, there's a slightly different problem: VS2013 has a strtof()
 * that returns the correct results for valid input, but may fail to report an
 * error for underflow or overflow, returning 0 instead. Work around that by
 * trying strtod() when strtof() returns 0.0 or [+-]Inf, and calling it an
 * error if the result differs. Also, strtof() doesn't handle subnormal input
 * well, so prefer to round the strtod() result in such cases. (Normally we'd
 * just say "too bad" if strtof() doesn't support subnormals, but since we're
 * already in here fixing stuff, we might as well do the best fix we can.)
 *
 * Cygwin has a strtof() which is literally just (float)strtod(), which means
 * we can't avoid the double-rounding problem; but using this wrapper does get
 * us proper over/underflow checks. (Also, if they fix their strtof(), the
 * wrapper doesn't break anything.)
 *
 * Test results on Mingw suggest that it has the same problem, though looking
 * at the code I can't figure out why.
 */
float
pg_strtof(const char *nptr, char **endptr)
{
	int			caller_errno = errno;
	float		fresult;

	errno = 0;
	fresult = (strtof) (nptr, endptr);
	if (errno)
	{
		/* On error, just return the error to the caller. */
		return fresult;
	}
	else if ((*endptr == nptr) || isnan(fresult) ||
			 ((fresult >= FLT_MIN || fresult <= -FLT_MIN) && !isinf(fresult)))
	{
		/*
		 * If we got nothing parseable, or if we got a non-0 non-subnormal
		 * finite value (or NaN) without error, then return that to the caller
		 * without error.
		 */
		errno = caller_errno;
		return fresult;
	}
	else
	{
		/*
		 * Try again. errno is already 0 here.
		 */
		double		dresult = strtod(nptr, NULL);

		if (errno)
		{
			/* On error, just return the error */
			return fresult;
		}
		else if ((dresult == 0.0 && fresult == 0.0) ||
				 (isinf(dresult) && isinf(fresult) && (fresult == dresult)))
		{
			/* both values are 0 or infinities of the same sign */
			errno = caller_errno;
			return fresult;
		}
		else if ((dresult > 0 && dresult <= FLT_MIN && (float) dresult != 0.0) ||
				 (dresult < 0 && dresult >= -FLT_MIN && (float) dresult != 0.0))
		{
			/* subnormal but nonzero value */
			errno = caller_errno;
			return (float) dresult;
		}
		else
		{
			errno = ERANGE;
			return fresult;
		}
	}
}

#endif
