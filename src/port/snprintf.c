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
 * 3. All advertising materials mentioning features or use of this software
 *	  must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
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
 */

/* might be in either frontend or backend */
#include "postgres_fe.h"

#include <sys/ioctl.h>
#include <sys/param.h>


/*
 * We do all internal arithmetic in the widest available integer type,
 * here called long_long (or ulong_long for unsigned).
 */
#ifdef HAVE_LONG_LONG_INT_64
typedef long long long_long;
typedef unsigned long long ulong_long;

#else
typedef long long_long;
typedef unsigned long ulong_long;
#endif

/*
**	SNPRINTF, VSNPRINT -- counted versions of printf
**
**	These versions have been grabbed off the net.  They have been
**	cleaned up to compile properly and support for .precision and
**	%lx has been added.
*/

/**************************************************************
 * Original:
 * Patrick Powell Tue Apr 11 09:48:21 PDT 1995
 * A bombproof version of doprnt (dopr) included.
 * Sigh.  This sort of thing is always nasty do deal with.	Note that
 * the version here does not include floating point. (now it does ... tgl)
 *
 * snprintf() is used instead of sprintf() as it does limit checks
 * for string length.  This covers a nasty loophole.
 *
 * The other functions are there to prevent NULL pointers from
 * causing nast effects.
 **************************************************************/

/*static char _id[] = "$Id: snprintf.c,v 1.1 2002/07/18 04:13:59 momjian Exp $";*/
static char *end;
static int	SnprfOverflow;

int			snprintf(char *str, size_t count, const char *fmt,...);
int			vsnprintf(char *str, size_t count, const char *fmt, va_list args);
static void dopr(char *buffer, const char *format, va_list args);

int
snprintf(char *str, size_t count, const char *fmt,...)
{
	int			len;
	va_list		args;

	va_start(args, fmt);
	len = vsnprintf(str, count, fmt, args);
	va_end(args);
	return len;
}


int
vsnprintf(char *str, size_t count, const char *fmt, va_list args)
{
	str[0] = '\0';
	end = str + count - 1;
	SnprfOverflow = 0;
	dopr(str, fmt, args);
	if (count > 0)
		end[0] = '\0';
	return strlen(str);
}

/*
 * dopr(): poor man's version of doprintf
 */

static void fmtstr(char *value, int ljust, int len, int zpad, int maxwidth);
static void fmtnum(long_long value, int base, int dosign, int ljust, int len, int zpad);
static void fmtfloat(double value, char type, int ljust, int len, int precision, int pointflag);
static void dostr(char *str, int cut);
static void dopr_outch(int c);

static char *output;


static void
dopr(char *buffer, const char *format, va_list args)
{
	int			ch;
	long_long	value;
	double		fvalue;
	int			longlongflag = 0;
	int			longflag = 0;
	int			pointflag = 0;
	int			maxwidth = 0;
	char	   *strvalue;
	int			ljust;
	int			len;
	int			zpad;

	output = buffer;
	while ((ch = *format++))
	{
		switch (ch)
		{
			case '%':
				ljust = len = zpad = maxwidth = 0;
				longflag = longlongflag = pointflag = 0;
		nextch:
				ch = *format++;
				switch (ch)
				{
					case 0:
						dostr("**end of format**", 0);
						*output = '\0';
						return;
					case '-':
						ljust = 1;
						goto nextch;
					case '0':	/* set zero padding if len not set */
						if (len == 0 && !pointflag)
							zpad = '0';
					case '1':
					case '2':
					case '3':
					case '4':
					case '5':
					case '6':
					case '7':
					case '8':
					case '9':
						if (pointflag)
							maxwidth = maxwidth * 10 + ch - '0';
						else
							len = len * 10 + ch - '0';
						goto nextch;
					case '*':
						if (pointflag)
							maxwidth = va_arg(args, int);
						else
							len = va_arg(args, int);
						goto nextch;
					case '.':
						pointflag = 1;
						goto nextch;
					case 'l':
						if (longflag)
							longlongflag = 1;
						else
							longflag = 1;
						goto nextch;
					case 'u':
					case 'U':
						/* fmtnum(value,base,dosign,ljust,len,zpad) */
						if (longflag)
						{
							if (longlongflag)
								value = va_arg(args, ulong_long);
							else
								value = va_arg(args, unsigned long);
						}
						else
							value = va_arg(args, unsigned int);
						fmtnum(value, 10, 0, ljust, len, zpad);
						break;
					case 'o':
					case 'O':
						/* fmtnum(value,base,dosign,ljust,len,zpad) */
						if (longflag)
						{
							if (longlongflag)
								value = va_arg(args, ulong_long);
							else
								value = va_arg(args, unsigned long);
						}
						else
							value = va_arg(args, unsigned int);
						fmtnum(value, 8, 0, ljust, len, zpad);
						break;
					case 'd':
					case 'D':
						if (longflag)
						{
							if (longlongflag)
								value = va_arg(args, long_long);
							else
								value = va_arg(args, long);
						}
						else
							value = va_arg(args, int);
						fmtnum(value, 10, 1, ljust, len, zpad);
						break;
					case 'x':
						if (longflag)
						{
							if (longlongflag)
								value = va_arg(args, ulong_long);
							else
								value = va_arg(args, unsigned long);
						}
						else
							value = va_arg(args, unsigned int);
						fmtnum(value, 16, 0, ljust, len, zpad);
						break;
					case 'X':
						if (longflag)
						{
							if (longlongflag)
								value = va_arg(args, ulong_long);
							else
								value = va_arg(args, unsigned long);
						}
						else
							value = va_arg(args, unsigned int);
						fmtnum(value, -16, 0, ljust, len, zpad);
						break;
					case 's':
						strvalue = va_arg(args, char *);
						if (maxwidth > 0 || !pointflag)
						{
							if (pointflag && len > maxwidth)
								len = maxwidth; /* Adjust padding */
							fmtstr(strvalue, ljust, len, zpad, maxwidth);
						}
						break;
					case 'c':
						ch = va_arg(args, int);
						dopr_outch(ch);
						break;
					case 'e':
					case 'E':
					case 'f':
					case 'g':
					case 'G':
						fvalue = va_arg(args, double);
						fmtfloat(fvalue, ch, ljust, len, maxwidth, pointflag);
						break;
					case '%':
						dopr_outch(ch);
						continue;
					default:
						dostr("???????", 0);
				}
				break;
			default:
				dopr_outch(ch);
				break;
		}
	}
	*output = '\0';
}

static void
fmtstr(char *value, int ljust, int len, int zpad, int maxwidth)
{
	int			padlen,
				strlen;			/* amount to pad */

	if (value == 0)
		value = "<NULL>";
	for (strlen = 0; value[strlen]; ++strlen);	/* strlen */
	if (strlen > maxwidth && maxwidth)
		strlen = maxwidth;
	padlen = len - strlen;
	if (padlen < 0)
		padlen = 0;
	if (ljust)
		padlen = -padlen;
	while (padlen > 0)
	{
		dopr_outch(' ');
		--padlen;
	}
	dostr(value, maxwidth);
	while (padlen < 0)
	{
		dopr_outch(' ');
		++padlen;
	}
}

static void
fmtnum(long_long value, int base, int dosign, int ljust, int len, int zpad)
{
	int			signvalue = 0;
	ulong_long	uvalue;
	char		convert[64];
	int			place = 0;
	int			padlen = 0;		/* amount to pad */
	int			caps = 0;

	/*
	 * DEBUGP(("value 0x%x, base %d, dosign %d, ljust %d, len %d, zpad
	 * %d\n", value, base, dosign, ljust, len, zpad ));
	 */
	uvalue = value;
	if (dosign)
	{
		if (value < 0)
		{
			signvalue = '-';
			uvalue = -value;
		}
	}
	if (base < 0)
	{
		caps = 1;
		base = -base;
	}
	do
	{
		convert[place++] = (caps ? "0123456789ABCDEF" : "0123456789abcdef")
			[uvalue % (unsigned) base];
		uvalue = (uvalue / (unsigned) base);
	} while (uvalue);
	convert[place] = 0;

	if (len < 0)
	{
		/* this could happen with a "*" width spec */
		ljust = 1;
		len = -len;
	}
	padlen = len - place;
	if (padlen < 0)
		padlen = 0;
	if (ljust)
		padlen = -padlen;

	/*
	 * DEBUGP(( "str '%s', place %d, sign %c, padlen %d\n",
	 * convert,place,signvalue,padlen));
	 */
	if (zpad && padlen > 0)
	{
		if (signvalue)
		{
			dopr_outch(signvalue);
			--padlen;
			signvalue = 0;
		}
		while (padlen > 0)
		{
			dopr_outch(zpad);
			--padlen;
		}
	}
	while (padlen > 0)
	{
		dopr_outch(' ');
		--padlen;
	}
	if (signvalue)
		dopr_outch(signvalue);
	while (place > 0)
		dopr_outch(convert[--place]);
	while (padlen < 0)
	{
		dopr_outch(' ');
		++padlen;
	}
}

static void
fmtfloat(double value, char type, int ljust, int len, int precision, int pointflag)
{
	char		fmt[32];
	char		convert[512];
	int			padlen = 0;		/* amount to pad */

	/* we rely on regular C library's sprintf to do the basic conversion */
	if (pointflag)
		sprintf(fmt, "%%.%d%c", precision, type);
	else
		sprintf(fmt, "%%%c", type);
	sprintf(convert, fmt, value);

	if (len < 0)
	{
		/* this could happen with a "*" width spec */
		ljust = 1;
		len = -len;
	}
	padlen = len - strlen(convert);
	if (padlen < 0)
		padlen = 0;
	if (ljust)
		padlen = -padlen;

	while (padlen > 0)
	{
		dopr_outch(' ');
		--padlen;
	}
	dostr(convert, 0);
	while (padlen < 0)
	{
		dopr_outch(' ');
		++padlen;
	}
}

static void
dostr(char *str, int cut)
{
	if (cut)
	{
		while (*str && cut-- > 0)
			dopr_outch(*str++);
	}
	else
	{
		while (*str)
			dopr_outch(*str++);
	}
}

static void
dopr_outch(int c)
{
#ifdef NOT_USED
	if (iscntrl((unsigned char) c) && c != '\n' && c != '\t')
	{
		c = '@' + (c & 0x1F);
		if (end == 0 || output < end)
			*output++ = '^';
	}
#endif
	if (end == 0 || output < end)
		*output++ = c;
	else
		SnprfOverflow++;
}
