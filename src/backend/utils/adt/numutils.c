/*-------------------------------------------------------------------------
 *
 * numutils.c
 *	  utility functions for I/O of built-in numeric types.
 *
 *		integer:				pg_atoi, pg_itoa, pg_ltoa
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/numutils.c,v 1.63 2004/04/01 22:51:31 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <math.h>
#include <limits.h>
#include <ctype.h>

#include "utils/builtins.h"

#ifndef INT_MAX
#define INT_MAX (0x7FFFFFFFL)
#endif
#ifndef INT_MIN
#define INT_MIN (-INT_MAX-1)
#endif
#ifndef SHRT_MAX
#define SHRT_MAX (0x7FFF)
#endif
#ifndef SHRT_MIN
#define SHRT_MIN (-SHRT_MAX-1)
#endif
#ifndef SCHAR_MAX
#define SCHAR_MAX (0x7F)
#endif
#ifndef SCHAR_MIN
#define SCHAR_MIN (-SCHAR_MAX-1)
#endif


/*
 * pg_atoi: convert string to integer
 *
 * 'size' is the sizeof() the desired integral result (1, 2, or 4 bytes).
 *
 * allows any number of leading or trailing whitespace characters.
 *
 * 'c' is the character that terminates the input string (after any
 * number of whitespace characters).
 *
 * Unlike plain atoi(), this will throw ereport() upon bad input format or
 * overflow.
 */
int32
pg_atoi(char *s, int size, int c)
{
	long		l;
	char	   *badp;

	/*
	 * Some versions of strtol treat the empty string as an error, but
	 * some seem not to.  Make an explicit test to be sure we catch it.
	 */
	if (s == NULL)
		elog(ERROR, "NULL pointer");
	if (*s == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for integer: \"%s\"",
						s)));

	errno = 0;
	l = strtol(s, &badp, 10);

	/* We made no progress parsing the string, so bail out */
	if (s == badp)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for integer: \"%s\"",
						s)));

	/*
	 * Skip any trailing whitespace; if anything but whitespace
	 * remains before the terminating character, bail out
	 */
	while (*badp != c && isspace((unsigned char) *badp))
		badp++;

	if (*badp != c)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for integer: \"%s\"",
						s)));

	switch (size)
	{
		case sizeof(int32):
			if (errno == ERANGE
#if defined(HAVE_LONG_INT_64)
			/* won't get ERANGE on these with 64-bit longs... */
				|| l < INT_MIN || l > INT_MAX
#endif
				)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("value \"%s\" is out of range for type integer", s)));
			break;
		case sizeof(int16):
			if (errno == ERANGE || l < SHRT_MIN || l > SHRT_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("value \"%s\" is out of range for type shortint", s)));
			break;
		case sizeof(int8):
			if (errno == ERANGE || l < SCHAR_MIN || l > SCHAR_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("value \"%s\" is out of range for 8-bit integer", s)));
			break;
		default:
			elog(ERROR, "unsupported result size: %d", size);
	}
	return (int32) l;
}

/*
 *		pg_itoa			- converts a short int to its string represention
 *
 *		Note:
 *				previously based on ~ingres/source/gutil/atoi.c
 *				now uses vendor's sprintf conversion
 */
void
pg_itoa(int16 i, char *a)
{
	sprintf(a, "%hd", (short) i);
}

/*
 *		pg_ltoa			- converts a long int to its string represention
 *
 *		Note:
 *				previously based on ~ingres/source/gutil/atoi.c
 *				now uses vendor's sprintf conversion
 */
void
pg_ltoa(int32 l, char *a)
{
	sprintf(a, "%d", l);
}
