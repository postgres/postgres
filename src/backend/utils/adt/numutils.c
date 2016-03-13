/*-------------------------------------------------------------------------
 *
 * numutils.c
 *	  utility functions for I/O of built-in numeric types.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/numutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <limits.h>
#include <ctype.h>

#include "utils/builtins.h"

/*
 * pg_atoi: convert string to integer
 *
 * allows any number of leading or trailing whitespace characters.
 *
 * 'size' is the sizeof() the desired integral result (1, 2, or 4 bytes).
 *
 * c, if not 0, is a terminator character that may appear after the
 * integer (plus whitespace).  If 0, the string must end after the integer.
 *
 * Unlike plain atoi(), this will throw ereport() upon bad input format or
 * overflow.
 */
int32
pg_atoi(const char *s, int size, int c)
{
	long		l;
	char	   *badp;

	/*
	 * Some versions of strtol treat the empty string as an error, but some
	 * seem not to.  Make an explicit test to be sure we catch it.
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
				errmsg("value \"%s\" is out of range for type smallint", s)));
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

	/*
	 * Skip any trailing whitespace; if anything but whitespace remains before
	 * the terminating character, bail out
	 */
	while (*badp && *badp != c && isspace((unsigned char) *badp))
		badp++;

	if (*badp && *badp != c)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for integer: \"%s\"",
						s)));

	return (int32) l;
}

/*
 * pg_itoa: converts a signed 16-bit integer to its string representation
 *
 * Caller must ensure that 'a' points to enough memory to hold the result
 * (at least 7 bytes, counting a leading sign and trailing NUL).
 *
 * It doesn't seem worth implementing this separately.
 */
void
pg_itoa(int16 i, char *a)
{
	pg_ltoa((int32) i, a);
}

/*
 * pg_ltoa: converts a signed 32-bit integer to its string representation
 *
 * Caller must ensure that 'a' points to enough memory to hold the result
 * (at least 12 bytes, counting a leading sign and trailing NUL).
 */
void
pg_ltoa(int32 value, char *a)
{
	char	   *start = a;
	bool		neg = false;

	/*
	 * Avoid problems with the most negative integer not being representable
	 * as a positive integer.
	 */
	if (value == (-2147483647 - 1))
	{
		memcpy(a, "-2147483648", 12);
		return;
	}
	else if (value < 0)
	{
		value = -value;
		neg = true;
	}

	/* Compute the result string backwards. */
	do
	{
		int32		remainder;
		int32		oldval = value;

		value /= 10;
		remainder = oldval - value * 10;
		*a++ = '0' + remainder;
	} while (value != 0);

	if (neg)
		*a++ = '-';

	/* Add trailing NUL byte, and back up 'a' to the last character. */
	*a-- = '\0';

	/* Reverse string. */
	while (start < a)
	{
		char		swap = *start;

		*start++ = *a;
		*a-- = swap;
	}
}

/*
 * pg_lltoa: convert a signed 64-bit integer to its string representation
 *
 * Caller must ensure that 'a' points to enough memory to hold the result
 * (at least MAXINT8LEN+1 bytes, counting a leading sign and trailing NUL).
 */
void
pg_lltoa(int64 value, char *a)
{
	char	   *start = a;
	bool		neg = false;

	/*
	 * Avoid problems with the most negative integer not being representable
	 * as a positive integer.
	 */
	if (value == PG_INT64_MIN)
	{
		memcpy(a, "-9223372036854775808", 21);
		return;
	}
	else if (value < 0)
	{
		value = -value;
		neg = true;
	}

	/* Compute the result string backwards. */
	do
	{
		int64		remainder;
		int64		oldval = value;

		value /= 10;
		remainder = oldval - value * 10;
		*a++ = '0' + remainder;
	} while (value != 0);

	if (neg)
		*a++ = '-';

	/* Add trailing NUL byte, and back up 'a' to the last character. */
	*a-- = '\0';

	/* Reverse string. */
	while (start < a)
	{
		char		swap = *start;

		*start++ = *a;
		*a-- = swap;
	}
}


/*
 * pg_ltostr_zeropad
 *		Converts 'value' into a decimal string representation stored at 'str'.
 *		'minwidth' specifies the minimum width of the result; any extra space
 *		is filled up by prefixing the number with zeros.
 *
 * Returns the ending address of the string result (the last character written
 * plus 1).  Note that no NUL terminator is written.
 *
 * The intended use-case for this function is to build strings that contain
 * multiple individual numbers, for example:
 *
 *	str = pg_ltostr_zeropad(str, hours, 2);
 *	*str++ = ':';
 *	str = pg_ltostr_zeropad(str, mins, 2);
 *	*str++ = ':';
 *	str = pg_ltostr_zeropad(str, secs, 2);
 *	*str = '\0';
 *
 * Note: Caller must ensure that 'str' points to enough memory to hold the
 * result.
 */
char *
pg_ltostr_zeropad(char *str, int32 value, int32 minwidth)
{
	char	   *start = str;
	char	   *end = &str[minwidth];
	int32		num = value;

	Assert(minwidth > 0);

	/*
	 * Handle negative numbers in a special way.  We can't just write a '-'
	 * prefix and reverse the sign as that would overflow for INT32_MIN.
	 */
	if (num < 0)
	{
		*start++ = '-';
		minwidth--;

		/*
		 * Build the number starting at the last digit.  Here remainder will
		 * be a negative number, so we must reverse the sign before adding '0'
		 * in order to get the correct ASCII digit.
		 */
		while (minwidth--)
		{
			int32		oldval = num;
			int32		remainder;

			num /= 10;
			remainder = oldval - num * 10;
			start[minwidth] = '0' - remainder;
		}
	}
	else
	{
		/* Build the number starting at the last digit */
		while (minwidth--)
		{
			int32		oldval = num;
			int32		remainder;

			num /= 10;
			remainder = oldval - num * 10;
			start[minwidth] = '0' + remainder;
		}
	}

	/*
	 * If minwidth was not high enough to fit the number then num won't have
	 * been divided down to zero.  We punt the problem to pg_ltostr(), which
	 * will generate a correct answer in the minimum valid width.
	 */
	if (num != 0)
		return pg_ltostr(str, value);

	/* Otherwise, return last output character + 1 */
	return end;
}

/*
 * pg_ltostr
 *		Converts 'value' into a decimal string representation stored at 'str'.
 *
 * Returns the ending address of the string result (the last character written
 * plus 1).  Note that no NUL terminator is written.
 *
 * The intended use-case for this function is to build strings that contain
 * multiple individual numbers, for example:
 *
 *	str = pg_ltostr(str, a);
 *	*str++ = ' ';
 *	str = pg_ltostr(str, b);
 *	*str = '\0';
 *
 * Note: Caller must ensure that 'str' points to enough memory to hold the
 * result.
 */
char *
pg_ltostr(char *str, int32 value)
{
	char	   *start;
	char	   *end;

	/*
	 * Handle negative numbers in a special way.  We can't just write a '-'
	 * prefix and reverse the sign as that would overflow for INT32_MIN.
	 */
	if (value < 0)
	{
		*str++ = '-';

		/* Mark the position we must reverse the string from. */
		start = str;

		/* Compute the result string backwards. */
		do
		{
			int32		oldval = value;
			int32		remainder;

			value /= 10;
			remainder = oldval - value * 10;
			/* As above, we expect remainder to be negative. */
			*str++ = '0' - remainder;
		} while (value != 0);
	}
	else
	{
		/* Mark the position we must reverse the string from. */
		start = str;

		/* Compute the result string backwards. */
		do
		{
			int32		oldval = value;
			int32		remainder;

			value /= 10;
			remainder = oldval - value * 10;
			*str++ = '0' + remainder;
		} while (value != 0);
	}

	/* Remember the end+1 and back up 'str' to the last character. */
	end = str--;

	/* Reverse string. */
	while (start < str)
	{
		char		swap = *start;

		*start++ = *str;
		*str-- = swap;
	}

	return end;
}

/*
 * pg_strtouint64
 *		Converts 'str' into an unsigned 64-bit integer.
 *
 * This has the identical API to strtoul(3), except that it will handle
 * 64-bit ints even where "long" is narrower than that.
 *
 * For the moment it seems sufficient to assume that the platform has
 * such a function somewhere; let's not roll our own.
 */
uint64
pg_strtouint64(const char *str, char **endptr, int base)
{
#ifdef _MSC_VER					/* MSVC only */
	return _strtoui64(str, endptr, base);
#elif defined(HAVE_STRTOULL) && SIZEOF_LONG < 8
	return strtoull(str, endptr, base);
#else
	return strtoul(str, endptr, base);
#endif
}
