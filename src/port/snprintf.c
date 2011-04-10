/*
 * Copyright (c) 1983, 1995, 1996 Eric P. Allman
 * Copyright (c) 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * src/port/snprintf.c
 */

#include "c.h"

#include <limits.h>
#ifndef WIN32
#include <sys/ioctl.h>
#endif
#include <sys/param.h>

#ifndef NL_ARGMAX
#define NL_ARGMAX 16
#endif


/*
 *	SNPRINTF, VSNPRINTF and friends
 *
 * These versions have been grabbed off the net.  They have been
 * cleaned up to compile properly and support for most of the Single Unix
 * Specification has been added.  Remaining unimplemented features are:
 *
 * 1. No locale support: the radix character is always '.' and the '
 * (single quote) format flag is ignored.
 *
 * 2. No support for the "%n" format specification.
 *
 * 3. No support for wide characters ("lc" and "ls" formats).
 *
 * 4. No support for "long double" ("Lf" and related formats).
 *
 * 5. Space and '#' flags are not implemented.
 *
 *
 * The result values of these functions are not the same across different
 * platforms.  This implementation is compatible with the Single Unix Spec:
 *
 * 1. -1 is returned only if processing is abandoned due to an invalid
 * parameter, such as incorrect format string.	(Although not required by
 * the spec, this happens only when no characters have yet been transmitted
 * to the destination.)
 *
 * 2. For snprintf and sprintf, 0 is returned if str == NULL or count == 0;
 * no data has been stored.
 *
 * 3. Otherwise, the number of bytes actually transmitted to the destination
 * is returned (excluding the trailing '\0' for snprintf and sprintf).
 *
 * For snprintf with nonzero count, the result cannot be more than count-1
 * (a trailing '\0' is always stored); it is not possible to distinguish
 * buffer overrun from exact fit.  This is unlike some implementations that
 * return the number of bytes that would have been needed for the complete
 * result string.
 */

/**************************************************************
 * Original:
 * Patrick Powell Tue Apr 11 09:48:21 PDT 1995
 * A bombproof version of doprnt (dopr) included.
 * Sigh.  This sort of thing is always nasty do deal with.	Note that
 * the version here does not include floating point. (now it does ... tgl)
 **************************************************************/

/* Prevent recursion */
#undef	vsnprintf
#undef	snprintf
#undef	sprintf
#undef	vfprintf
#undef	fprintf
#undef	printf

/* Info about where the formatted output is going */
typedef struct
{
	char	   *bufptr;			/* next buffer output position */
	char	   *bufstart;		/* first buffer element */
	char	   *bufend;			/* last buffer element, or NULL */
	/* bufend == NULL is for sprintf, where we assume buf is big enough */
	FILE	   *stream;			/* eventual output destination, or NULL */
	int			nchars;			/* # chars already sent to stream */
} PrintfTarget;

/*
 * Info about the type and value of a formatting parameter.  Note that we
 * don't currently support "long double", "wint_t", or "wchar_t *" data,
 * nor the '%n' formatting code; else we'd need more types.  Also, at this
 * level we need not worry about signed vs unsigned values.
 */
typedef enum
{
	ATYPE_NONE = 0,
	ATYPE_INT,
	ATYPE_LONG,
	ATYPE_LONGLONG,
	ATYPE_DOUBLE,
	ATYPE_CHARPTR
} PrintfArgType;

typedef union
{
	int			i;
	long		l;
	int64		ll;
	double		d;
	char	   *cptr;
} PrintfArgValue;


static void flushbuffer(PrintfTarget *target);
static int	dopr(PrintfTarget *target, const char *format, va_list args);


int
pg_vsnprintf(char *str, size_t count, const char *fmt, va_list args)
{
	PrintfTarget target;

	if (str == NULL || count == 0)
		return 0;
	target.bufstart = target.bufptr = str;
	target.bufend = str + count - 1;
	target.stream = NULL;
	/* target.nchars is unused in this case */
	if (dopr(&target, fmt, args))
	{
		*(target.bufptr) = '\0';
		errno = EINVAL;			/* bad format */
		return -1;
	}
	*(target.bufptr) = '\0';
	return target.bufptr - target.bufstart;
}

int
pg_snprintf(char *str, size_t count, const char *fmt,...)
{
	int			len;
	va_list		args;

	va_start(args, fmt);
	len = pg_vsnprintf(str, count, fmt, args);
	va_end(args);
	return len;
}

static int
pg_vsprintf(char *str, const char *fmt, va_list args)
{
	PrintfTarget target;

	if (str == NULL)
		return 0;
	target.bufstart = target.bufptr = str;
	target.bufend = NULL;
	target.stream = NULL;
	/* target.nchars is unused in this case */
	if (dopr(&target, fmt, args))
	{
		*(target.bufptr) = '\0';
		errno = EINVAL;			/* bad format */
		return -1;
	}
	*(target.bufptr) = '\0';
	return target.bufptr - target.bufstart;
}

int
pg_sprintf(char *str, const char *fmt,...)
{
	int			len;
	va_list		args;

	va_start(args, fmt);
	len = pg_vsprintf(str, fmt, args);
	va_end(args);
	return len;
}

int
pg_vfprintf(FILE *stream, const char *fmt, va_list args)
{
	PrintfTarget target;
	char		buffer[1024];	/* size is arbitrary */

	if (stream == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	target.bufstart = target.bufptr = buffer;
	target.bufend = buffer + sizeof(buffer) - 1;
	target.stream = stream;
	target.nchars = 0;
	if (dopr(&target, fmt, args))
	{
		errno = EINVAL;			/* bad format */
		return -1;
	}
	/* dump any remaining buffer contents */
	flushbuffer(&target);
	return target.nchars;
}

int
pg_fprintf(FILE *stream, const char *fmt,...)
{
	int			len;
	va_list		args;

	va_start(args, fmt);
	len = pg_vfprintf(stream, fmt, args);
	va_end(args);
	return len;
}

int
pg_printf(const char *fmt,...)
{
	int			len;
	va_list		args;

	va_start(args, fmt);
	len = pg_vfprintf(stdout, fmt, args);
	va_end(args);
	return len;
}

/* call this only when stream is defined */
static void
flushbuffer(PrintfTarget *target)
{
	size_t		nc = target->bufptr - target->bufstart;

	if (nc > 0)
		target->nchars += fwrite(target->bufstart, 1, nc, target->stream);
	target->bufptr = target->bufstart;
}


static void fmtstr(char *value, int leftjust, int minlen, int maxwidth,
	   int pointflag, PrintfTarget *target);
static void fmtptr(void *value, PrintfTarget *target);
static void fmtint(int64 value, char type, int forcesign,
	   int leftjust, int minlen, int zpad, int precision, int pointflag,
	   PrintfTarget *target);
static void fmtchar(int value, int leftjust, int minlen, PrintfTarget *target);
static void fmtfloat(double value, char type, int forcesign,
		 int leftjust, int minlen, int zpad, int precision, int pointflag,
		 PrintfTarget *target);
static void dostr(const char *str, int slen, PrintfTarget *target);
static void dopr_outch(int c, PrintfTarget *target);
static int	adjust_sign(int is_negative, int forcesign, int *signvalue);
static void adjust_padlen(int minlen, int vallen, int leftjust, int *padlen);
static void leading_pad(int zpad, int *signvalue, int *padlen,
			PrintfTarget *target);
static void trailing_pad(int *padlen, PrintfTarget *target);


/*
 * dopr(): poor man's version of doprintf
 */
static int
dopr(PrintfTarget *target, const char *format, va_list args)
{
	const char *format_start = format;
	int			ch;
	bool		have_dollar;
	bool		have_non_dollar;
	bool		have_star;
	bool		afterstar;
	int			accum;
	int			longlongflag;
	int			longflag;
	int			pointflag;
	int			leftjust;
	int			fieldwidth;
	int			precision;
	int			zpad;
	int			forcesign;
	int			last_dollar;
	int			fmtpos;
	int			cvalue;
	int64		numvalue;
	double		fvalue;
	char	   *strvalue;
	int			i;
	PrintfArgType argtypes[NL_ARGMAX + 1];
	PrintfArgValue argvalues[NL_ARGMAX + 1];

	/*
	 * Parse the format string to determine whether there are %n$ format
	 * specs, and identify the types and order of the format parameters.
	 */
	have_dollar = have_non_dollar = false;
	last_dollar = 0;
	MemSet(argtypes, 0, sizeof(argtypes));

	while ((ch = *format++) != '\0')
	{
		if (ch != '%')
			continue;
		longflag = longlongflag = pointflag = 0;
		fmtpos = accum = 0;
		afterstar = false;
nextch1:
		ch = *format++;
		if (ch == '\0')
			break;				/* illegal, but we don't complain */
		switch (ch)
		{
			case '-':
			case '+':
				goto nextch1;
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				accum = accum * 10 + (ch - '0');
				goto nextch1;
			case '.':
				pointflag = 1;
				accum = 0;
				goto nextch1;
			case '*':
				if (afterstar)
					have_non_dollar = true;		/* multiple stars */
				afterstar = true;
				accum = 0;
				goto nextch1;
			case '$':
				have_dollar = true;
				if (accum <= 0 || accum > NL_ARGMAX)
					return -1;
				if (afterstar)
				{
					if (argtypes[accum] &&
						argtypes[accum] != ATYPE_INT)
						return -1;
					argtypes[accum] = ATYPE_INT;
					last_dollar = Max(last_dollar, accum);
					afterstar = false;
				}
				else
					fmtpos = accum;
				accum = 0;
				goto nextch1;
			case 'l':
				if (longflag)
					longlongflag = 1;
				else
					longflag = 1;
				goto nextch1;
			case 'h':
			case '\'':
				/* ignore these */
				goto nextch1;
			case 'd':
			case 'i':
			case 'o':
			case 'u':
			case 'x':
			case 'X':
				if (fmtpos)
				{
					PrintfArgType atype;

					if (longlongflag)
						atype = ATYPE_LONGLONG;
					else if (longflag)
						atype = ATYPE_LONG;
					else
						atype = ATYPE_INT;
					if (argtypes[fmtpos] &&
						argtypes[fmtpos] != atype)
						return -1;
					argtypes[fmtpos] = atype;
					last_dollar = Max(last_dollar, fmtpos);
				}
				else
					have_non_dollar = true;
				break;
			case 'c':
				if (fmtpos)
				{
					if (argtypes[fmtpos] &&
						argtypes[fmtpos] != ATYPE_INT)
						return -1;
					argtypes[fmtpos] = ATYPE_INT;
					last_dollar = Max(last_dollar, fmtpos);
				}
				else
					have_non_dollar = true;
				break;
			case 's':
			case 'p':
				if (fmtpos)
				{
					if (argtypes[fmtpos] &&
						argtypes[fmtpos] != ATYPE_CHARPTR)
						return -1;
					argtypes[fmtpos] = ATYPE_CHARPTR;
					last_dollar = Max(last_dollar, fmtpos);
				}
				else
					have_non_dollar = true;
				break;
			case 'e':
			case 'E':
			case 'f':
			case 'g':
			case 'G':
				if (fmtpos)
				{
					if (argtypes[fmtpos] &&
						argtypes[fmtpos] != ATYPE_DOUBLE)
						return -1;
					argtypes[fmtpos] = ATYPE_DOUBLE;
					last_dollar = Max(last_dollar, fmtpos);
				}
				else
					have_non_dollar = true;
				break;
			case '%':
				break;
		}

		/*
		 * If we finish the spec with afterstar still set, there's a
		 * non-dollar star in there.
		 */
		if (afterstar)
			have_non_dollar = true;
	}

	/* Per spec, you use either all dollar or all not. */
	if (have_dollar && have_non_dollar)
		return -1;

	/*
	 * In dollar mode, collect the arguments in physical order.
	 */
	for (i = 1; i <= last_dollar; i++)
	{
		switch (argtypes[i])
		{
			case ATYPE_NONE:
				return -1;		/* invalid format */
			case ATYPE_INT:
				argvalues[i].i = va_arg(args, int);
				break;
			case ATYPE_LONG:
				argvalues[i].l = va_arg(args, long);
				break;
			case ATYPE_LONGLONG:
				argvalues[i].ll = va_arg(args, int64);
				break;
			case ATYPE_DOUBLE:
				argvalues[i].d = va_arg(args, double);
				break;
			case ATYPE_CHARPTR:
				argvalues[i].cptr = va_arg(args, char *);
				break;
		}
	}

	/*
	 * At last we can parse the format for real.
	 */
	format = format_start;
	while ((ch = *format++) != '\0')
	{
		if (ch != '%')
		{
			dopr_outch(ch, target);
			continue;
		}
		fieldwidth = precision = zpad = leftjust = forcesign = 0;
		longflag = longlongflag = pointflag = 0;
		fmtpos = accum = 0;
		have_star = afterstar = false;
nextch2:
		ch = *format++;
		if (ch == '\0')
			break;				/* illegal, but we don't complain */
		switch (ch)
		{
			case '-':
				leftjust = 1;
				goto nextch2;
			case '+':
				forcesign = 1;
				goto nextch2;
			case '0':
				/* set zero padding if no nonzero digits yet */
				if (accum == 0 && !pointflag)
					zpad = '0';
				/* FALL THRU */
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				accum = accum * 10 + (ch - '0');
				goto nextch2;
			case '.':
				if (have_star)
					have_star = false;
				else
					fieldwidth = accum;
				pointflag = 1;
				accum = 0;
				goto nextch2;
			case '*':
				if (have_dollar)
				{
					/* process value after reading n$ */
					afterstar = true;
				}
				else
				{
					/* fetch and process value now */
					int			starval = va_arg(args, int);

					if (pointflag)
					{
						precision = starval;
						if (precision < 0)
						{
							precision = 0;
							pointflag = 0;
						}
					}
					else
					{
						fieldwidth = starval;
						if (fieldwidth < 0)
						{
							leftjust = 1;
							fieldwidth = -fieldwidth;
						}
					}
				}
				have_star = true;
				accum = 0;
				goto nextch2;
			case '$':
				if (afterstar)
				{
					/* fetch and process star value */
					int			starval = argvalues[accum].i;

					if (pointflag)
					{
						precision = starval;
						if (precision < 0)
						{
							precision = 0;
							pointflag = 0;
						}
					}
					else
					{
						fieldwidth = starval;
						if (fieldwidth < 0)
						{
							leftjust = 1;
							fieldwidth = -fieldwidth;
						}
					}
					afterstar = false;
				}
				else
					fmtpos = accum;
				accum = 0;
				goto nextch2;
			case 'l':
				if (longflag)
					longlongflag = 1;
				else
					longflag = 1;
				goto nextch2;
			case 'h':
			case '\'':
				/* ignore these */
				goto nextch2;
			case 'd':
			case 'i':
				if (!have_star)
				{
					if (pointflag)
						precision = accum;
					else
						fieldwidth = accum;
				}
				if (have_dollar)
				{
					if (longlongflag)
						numvalue = argvalues[fmtpos].ll;
					else if (longflag)
						numvalue = argvalues[fmtpos].l;
					else
						numvalue = argvalues[fmtpos].i;
				}
				else
				{
					if (longlongflag)
						numvalue = va_arg(args, int64);
					else if (longflag)
						numvalue = va_arg(args, long);
					else
						numvalue = va_arg(args, int);
				}
				fmtint(numvalue, ch, forcesign, leftjust, fieldwidth, zpad,
					   precision, pointflag, target);
				break;
			case 'o':
			case 'u':
			case 'x':
			case 'X':
				if (!have_star)
				{
					if (pointflag)
						precision = accum;
					else
						fieldwidth = accum;
				}
				if (have_dollar)
				{
					if (longlongflag)
						numvalue = (uint64) argvalues[fmtpos].ll;
					else if (longflag)
						numvalue = (unsigned long) argvalues[fmtpos].l;
					else
						numvalue = (unsigned int) argvalues[fmtpos].i;
				}
				else
				{
					if (longlongflag)
						numvalue = (uint64) va_arg(args, int64);
					else if (longflag)
						numvalue = (unsigned long) va_arg(args, long);
					else
						numvalue = (unsigned int) va_arg(args, int);
				}
				fmtint(numvalue, ch, forcesign, leftjust, fieldwidth, zpad,
					   precision, pointflag, target);
				break;
			case 'c':
				if (!have_star)
				{
					if (pointflag)
						precision = accum;
					else
						fieldwidth = accum;
				}
				if (have_dollar)
					cvalue = (unsigned char) argvalues[fmtpos].i;
				else
					cvalue = (unsigned char) va_arg(args, int);
				fmtchar(cvalue, leftjust, fieldwidth, target);
				break;
			case 's':
				if (!have_star)
				{
					if (pointflag)
						precision = accum;
					else
						fieldwidth = accum;
				}
				if (have_dollar)
					strvalue = argvalues[fmtpos].cptr;
				else
					strvalue = va_arg(args, char *);
				fmtstr(strvalue, leftjust, fieldwidth, precision, pointflag,
					   target);
				break;
			case 'p':
				/* fieldwidth/leftjust are ignored ... */
				if (have_dollar)
					strvalue = argvalues[fmtpos].cptr;
				else
					strvalue = va_arg(args, char *);
				fmtptr((void *) strvalue, target);
				break;
			case 'e':
			case 'E':
			case 'f':
			case 'g':
			case 'G':
				if (!have_star)
				{
					if (pointflag)
						precision = accum;
					else
						fieldwidth = accum;
				}
				if (have_dollar)
					fvalue = argvalues[fmtpos].d;
				else
					fvalue = va_arg(args, double);
				fmtfloat(fvalue, ch, forcesign, leftjust,
						 fieldwidth, zpad,
						 precision, pointflag,
						 target);
				break;
			case '%':
				dopr_outch('%', target);
				break;
		}
	}

	return 0;
}

static size_t
pg_strnlen(const char *str, size_t maxlen)
{
	const char *p = str;

	while (maxlen-- > 0 && *p)
		p++;
	return p - str;
}

static void
fmtstr(char *value, int leftjust, int minlen, int maxwidth,
	   int pointflag, PrintfTarget *target)
{
	int			padlen,
				vallen;			/* amount to pad */

	/*
	 * If a maxwidth (precision) is specified, we must not fetch more bytes
	 * than that.
	 */
	if (pointflag)
		vallen = pg_strnlen(value, maxwidth);
	else
		vallen = strlen(value);

	adjust_padlen(minlen, vallen, leftjust, &padlen);

	while (padlen > 0)
	{
		dopr_outch(' ', target);
		--padlen;
	}

	dostr(value, vallen, target);

	trailing_pad(&padlen, target);
}

static void
fmtptr(void *value, PrintfTarget *target)
{
	int			vallen;
	char		convert[64];

	/* we rely on regular C library's sprintf to do the basic conversion */
	vallen = sprintf(convert, "%p", value);

	dostr(convert, vallen, target);
}

static void
fmtint(int64 value, char type, int forcesign, int leftjust,
	   int minlen, int zpad, int precision, int pointflag,
	   PrintfTarget *target)
{
	uint64		base;
	int			dosign;
	const char *cvt = "0123456789abcdef";
	int			signvalue = 0;
	char		convert[64];
	int			vallen = 0;
	int			padlen = 0;		/* amount to pad */
	int			zeropad;		/* extra leading zeroes */

	switch (type)
	{
		case 'd':
		case 'i':
			base = 10;
			dosign = 1;
			break;
		case 'o':
			base = 8;
			dosign = 0;
			break;
		case 'u':
			base = 10;
			dosign = 0;
			break;
		case 'x':
			base = 16;
			dosign = 0;
			break;
		case 'X':
			cvt = "0123456789ABCDEF";
			base = 16;
			dosign = 0;
			break;
		default:
			return;				/* keep compiler quiet */
	}

	/* Handle +/- */
	if (dosign && adjust_sign((value < 0), forcesign, &signvalue))
		value = -value;

	/*
	 * SUS: the result of converting 0 with an explicit precision of 0 is no
	 * characters
	 */
	if (value == 0 && pointflag && precision == 0)
		vallen = 0;
	else
	{
		/* make integer string */
		uint64		uvalue = (uint64) value;

		do
		{
			convert[vallen++] = cvt[uvalue % base];
			uvalue = uvalue / base;
		} while (uvalue);
	}

	zeropad = Max(0, precision - vallen);

	adjust_padlen(minlen, vallen + zeropad, leftjust, &padlen);

	leading_pad(zpad, &signvalue, &padlen, target);

	while (zeropad-- > 0)
		dopr_outch('0', target);

	while (vallen > 0)
		dopr_outch(convert[--vallen], target);

	trailing_pad(&padlen, target);
}

static void
fmtchar(int value, int leftjust, int minlen, PrintfTarget *target)
{
	int			padlen = 0;		/* amount to pad */

	adjust_padlen(minlen, 1, leftjust, &padlen);

	while (padlen > 0)
	{
		dopr_outch(' ', target);
		--padlen;
	}

	dopr_outch(value, target);

	trailing_pad(&padlen, target);
}

static void
fmtfloat(double value, char type, int forcesign, int leftjust,
		 int minlen, int zpad, int precision, int pointflag,
		 PrintfTarget *target)
{
	int			signvalue = 0;
	int			vallen;
	char		fmt[32];
	char		convert[512];
	int			padlen = 0;		/* amount to pad */

	/* we rely on regular C library's sprintf to do the basic conversion */
	if (pointflag)
		sprintf(fmt, "%%.%d%c", precision, type);
	else
		sprintf(fmt, "%%%c", type);

	if (adjust_sign((value < 0), forcesign, &signvalue))
		value = -value;

	vallen = sprintf(convert, fmt, value);

	adjust_padlen(minlen, vallen, leftjust, &padlen);

	leading_pad(zpad, &signvalue, &padlen, target);

	dostr(convert, vallen, target);

	trailing_pad(&padlen, target);
}

static void
dostr(const char *str, int slen, PrintfTarget *target)
{
	while (slen > 0)
	{
		int			avail;

		if (target->bufend != NULL)
			avail = target->bufend - target->bufptr;
		else
			avail = slen;
		if (avail <= 0)
		{
			/* buffer full, can we dump to stream? */
			if (target->stream == NULL)
				return;			/* no, lose the data */
			flushbuffer(target);
			continue;
		}
		avail = Min(avail, slen);
		memmove(target->bufptr, str, avail);
		target->bufptr += avail;
		str += avail;
		slen -= avail;
	}
}

static void
dopr_outch(int c, PrintfTarget *target)
{
	if (target->bufend != NULL && target->bufptr >= target->bufend)
	{
		/* buffer full, can we dump to stream? */
		if (target->stream == NULL)
			return;				/* no, lose the data */
		flushbuffer(target);
	}
	*(target->bufptr++) = c;
}


static int
adjust_sign(int is_negative, int forcesign, int *signvalue)
{
	if (is_negative)
	{
		*signvalue = '-';
		return true;
	}
	else if (forcesign)
		*signvalue = '+';
	return false;
}


static void
adjust_padlen(int minlen, int vallen, int leftjust, int *padlen)
{
	*padlen = minlen - vallen;
	if (*padlen < 0)
		*padlen = 0;
	if (leftjust)
		*padlen = -(*padlen);
}


static void
leading_pad(int zpad, int *signvalue, int *padlen, PrintfTarget *target)
{
	if (*padlen > 0 && zpad)
	{
		if (*signvalue)
		{
			dopr_outch(*signvalue, target);
			--(*padlen);
			*signvalue = 0;
		}
		while (*padlen > 0)
		{
			dopr_outch(zpad, target);
			--(*padlen);
		}
	}
	while (*padlen > (*signvalue != 0))
	{
		dopr_outch(' ', target);
		--(*padlen);
	}
	if (*signvalue)
	{
		dopr_outch(*signvalue, target);
		if (*padlen > 0)
			--(*padlen);
		else if (*padlen < 0)
			++(*padlen);
	}
}


static void
trailing_pad(int *padlen, PrintfTarget *target)
{
	while (*padlen < 0)
	{
		dopr_outch(' ', target);
		++(*padlen);
	}
}
