/*-------------------------------------------------------------------------
 *
 * numutils.c
 *	  utility functions for I/O of built-in numeric types.
 *
 *		integer:				pg_itoa, pg_ltoa
 *		floating point:			ftoa, atof1
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/numutils.c,v 1.58 2003/09/25 06:58:04 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <math.h>
#include <limits.h>

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
 * size is the sizeof() the desired integral result (1, 2, or 4 bytes).
 *
 * c, if not 0, is the terminator character that may appear after the
 * integer.  If 0, the string must end after the integer.
 *
 * Unlike plain atoi(), this will throw ereport() upon bad input format or
 * overflow.
 */
int32
pg_atoi(char *s, int size, int c)
{
	long		l;
	char	   *badp = NULL;

	/*
	 * Some versions of strtol treat the empty string as an error, but
	 * some seem not to.  Make an explicit test to be sure we catch it.
	 */
	if (s == (char *) NULL)
		elog(ERROR, "NULL pointer");
	if (*s == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for integer: \"%s\"",
						s)));

	errno = 0;
	l = strtol(s, &badp, 10);

	/*
	 * strtol() normally only sets ERANGE.	On some systems it also may
	 * set EINVAL, which simply means it couldn't parse the input string.
	 * This is handled by the second "if" consistent across platforms.
	 */
	if (errno && errno != ERANGE && errno != EINVAL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for integer: \"%s\"",
						s)));
	if (badp && *badp && *badp != c)
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

/*
 **  ftoa		- FLOATING POINT TO ASCII CONVERSION
 **
 **		CODE derived from ingres, ~ingres/source/gutil/ftoa.c
 **
 **		'Value' is converted to an ascii character string and stored
 **		into 'ascii'.  Ascii should have room for at least 'width' + 1
 **		characters.  'Width' is the width of the output field (max).
 **		'Prec' is the number of characters to put after the decimal
 **		point.	The format of the output string is controlled by
 **		'format'.
 **
 **		'Format' can be:
 **				e or E: "E" format output
 **				f or F:  "F" format output
 **				g or G:  "F" format output if it will fit, otherwise
 **						use "E" format.
 **				n or N:  same as G, but decimal points will not always
 **						be aligned.
 **
 **		If 'format' is upper case, the "E" comes out in upper case;
 **		otherwise it comes out in lower case.
 **
 **		When the field width is not big enough, it fills the field with
 **		stars ("*****") and returns zero.  Normal return is the width
 **		of the output field (sometimes shorter than 'width').
 */
#ifdef NOT_USED
int
ftoa(double value, char *ascii, int width, int prec1, char format)
{
#ifndef HAVE_FCVT
	char		out[256];
	char		fmt[256];
	int			ret;

	sprintf(fmt, "%%%d.%d%c", width, prec1, format);
	sprintf(out, fmt, value);
	if ((ret = strlen(out)) > width)
	{
		MemSet(ascii, '*', width - 2);
		ascii[width] = 0;
		return 0;
	}
	strcpy(ascii, out);
	return ret;
#else
	auto int	expon;
	auto int	sign;
	int			avail = 0;
	char	   *a = NULL;
	char	   *p = NULL;
	char		mode;
	int			lowercase;
	int			prec;

/*	  extern char		*ecvt(), *fcvt();*/

	prec = prec1;
	mode = format;
	lowercase = 'a' - 'A';
	if (mode >= 'a')
		mode -= 'a' - 'A';
	else
		lowercase = 0;

	if (mode != 'E')
	{
		/* try 'F' style output */
		p = fcvt(value, prec, &expon, &sign);
		avail = width;
		a = ascii;

		/* output sign */
		if (sign)
		{
			avail--;
			*a++ = '-';
		}

		/* output '0' before the decimal point */
		if (expon <= 0)
		{
			*a++ = '0';
			avail--;
		}

		/* compute space length left after dec pt and fraction */
		avail -= prec + 1;
		if (mode == 'G')
			avail -= 4;

		if (avail >= expon)
		{

			/* it fits.  output */
			while (expon > 0)
			{
				/* output left of dp */
				expon--;
				if (*p)
					*a++ = *p++;
				else
					*a++ = '0';
			}

			/* output fraction (right of dec pt) */
			avail = expon;
			goto frac_out;
		}
		/* won't fit; let's hope for G format */
	}

	if (mode != 'F')
	{
		/* try to do E style output */
		p = ecvt(value, prec + 1, &expon, &sign);
		avail = width - 5;
		a = ascii;

		/* output the sign */
		if (sign)
		{
			*a++ = '-';
			avail--;
		}
	}

	/* check for field too small */
	if (mode == 'F' || avail < prec)
	{
		/* sorry joker, you lose */
		a = ascii;
		for (avail = width; avail > 0; avail--)
			*a++ = '*';
		*a = 0;
		return 0;
	}

	/* it fits; output the number */
	mode = 'E';

	/* output the LHS single digit */
	*a++ = *p++;
	expon--;

	/* output the rhs */
	avail = 1;

frac_out:
	*a++ = '.';
	while (prec > 0)
	{
		prec--;
		if (avail < 0)
		{
			avail++;
			*a++ = '0';
		}
		else
		{
			if (*p)
				*a++ = *p++;
			else
				*a++ = '0';
		}
	}

	/* output the exponent */
	if (mode == 'E')
	{
		*a++ = 'E' + lowercase;
		if (expon < 0)
		{
			*a++ = '-';
			expon = -expon;
		}
		else
			*a++ = '+';
		*a++ = (expon / 10) % 10 + '0';
		*a++ = expon % 10 + '0';
	}

	/* output spaces on the end in G format */
	if (mode == 'G')
	{
		*a++ = ' ';
		*a++ = ' ';
		*a++ = ' ';
		*a++ = ' ';
	}

	/* finally, we can return */
	*a = 0;
	avail = a - ascii;
	return avail;
#endif
}
#endif

/*
 **   atof1		- ASCII TO FLOATING CONVERSION
 **
 **		CODE derived from ~ingres/source/gutil/atof.c
 **
 **		Converts the string 'str' to floating point and stores the
 **		result into the cell pointed to by 'val'.
 **
 **		The syntax which it accepts is pretty much what you would
 **		expect.  Basically, it is:
 **				{<sp>} [+|-] {<sp>} {<digit>} [.{digit}] {<sp>} [<exp>]
 **		where <exp> is "e" or "E" followed by an integer, <sp> is a
 **		space character, <digit> is zero through nine, [] is zero or
 **		one, and {} is zero or more.
 **
 **		Parameters:
 **				str -- string to convert.
 **				val -- pointer to place to put the result (which
 **						must be type double).
 **
 **		Returns:
 **				zero -- ok.
 **				-1 -- syntax error.
 **				+1 -- overflow (not implemented).
 **
 **		Side Effects:
 **				clobbers *val.
 */
#ifdef NOT_USED
int
atof1(char *str, double *val)
{
	char	   *p;
	double		v;
	double		fact;
	int			minus;
	char		c;
	int			expon;
	int			gotmant;

	v = 0.0;
	p = str;
	minus = 0;

	/* skip leading blanks */
	while ((c = *p) != '\0')
	{
		if (c != ' ')
			break;
		p++;
	}

	/* handle possible sign */
	switch (c)
	{
		case '-':
			minus++;

		case '+':
			p++;
	}

	/* skip blanks after sign */
	while ((c = *p) != '\0')
	{
		if (c != ' ')
			break;
		p++;
	}

	/* start collecting the number to the decimal point */
	gotmant = 0;
	for (;;)
	{
		c = *p;
		if (c < '0' || c > '9')
			break;
		v = v * 10.0 + (c - '0');
		gotmant++;
		p++;
	}

	/* check for fractional part */
	if (c == '.')
	{
		fact = 1.0;
		for (;;)
		{
			c = *++p;
			if (c < '0' || c > '9')
				break;
			fact *= 0.1;
			v += (c - '0') * fact;
			gotmant++;
		}
	}

	/* skip blanks before possible exponent */
	while ((c = *p) != '\0')
	{
		if (c != ' ')
			break;
		p++;
	}

	/* test for exponent */
	if (c == 'e' || c == 'E')
	{
		p++;
		expon = pg_atoi(p, sizeof(expon), '\0');
		if (!gotmant)
			v = 1.0;
		fact = expon;
		v *= pow(10.0, fact);
	}
	else
	{
		/* if no exponent, then nothing */
		if (c != 0)
			return -1;
	}

	/* store the result and exit */
	if (minus)
		v = -v;
	*val = v;
	return 0;
}

#endif
