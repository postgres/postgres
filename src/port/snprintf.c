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

#ifdef ENABLE_THREAD_SAFETY
#error	The replacement snprintf() is not thread-safe.	\
Your platform must have a thread-safe snprintf() to compile with threads.
#endif

#ifndef WIN32
#include <sys/ioctl.h>
#endif
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

#ifndef NL_ARGMAX
#define NL_ARGMAX 4096
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
 * causing nasty effects.
 **************************************************************/

/*static char _id[] = "$PostgreSQL: pgsql/src/port/snprintf.c,v 1.8 2005/03/01 00:38:11 momjian Exp $";*/

int			snprintf(char *str, size_t count, const char *fmt,...);
int			vsnprintf(char *str, size_t count, const char *fmt, va_list args);
int			printf(const char *format, ...);
static void 		dopr(char *buffer, const char *format, va_list args, char *end);

int
printf(const char *fmt,...)
{
	int			len;
	va_list			args;
	char*		buffer[4096];
	char*			p;

	va_start(args, fmt);
	len = vsnprintf((char*)buffer, (size_t)4096, fmt, args);
	va_end(args);
	p = (char*)buffer;
	for(;*p;p++)
		putchar(*p);
	return len;
}

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
	char *end;
	str[0] = '\0';
	end = str + count - 1;
	dopr(str, fmt, args, end);
	if (count > 0)
		end[0] = '\0';
	return strlen(str);
}

/*
 * dopr(): poor man's version of doprintf
 */

static void fmtstr(char *value, int ljust, int len, int zpad, int maxwidth, char *end);
static void fmtnum(long_long value, int base, int dosign, int ljust, int len, int zpad, char *end);
static void fmtfloat(double value, char type, int ljust, int len, int precision, int pointflag, char *end);
static void dostr(char *str, int cut, char *end);
static void dopr_outch(int c, char *end);

static char *output;

#define	FMTSTR		1
#define	FMTNUM		2
#define	FMTFLOAT	3
#define	FMTCHAR		4

static void
dopr(char *buffer, const char *format, va_list args, char *end)
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
	int			i;
	const char*		format_save;
	const char*		fmtbegin;
	int			fmtpos = 1;
	int			realpos = 0;
	int			position;
	static struct{
		const char*	fmtbegin;
		const char*	fmtend;
		void*	value;
		long_long	numvalue;
		double	fvalue;
		int	charvalue;
		int	ljust;
		int	len;
		int	zpad;
		int	maxwidth;
		int	base;
		int	dosign;
		char	type;
		int	precision;
		int	pointflag;
		char	func;
		int	realpos;
	} fmtpar[NL_ARGMAX+1], *fmtparptr[NL_ARGMAX+1];


	format_save = format;
	output = buffer;
	while ((ch = *format++))
	{
		switch (ch)
		{
			case '%':
				ljust = len = zpad = maxwidth = 0;
				longflag = longlongflag = pointflag = 0;
				fmtbegin = format - 1;
				realpos = 0;
				position = 0;
		nextch:
				ch = *format++;
				switch (ch)
				{
					case 0:
						goto performpr;
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
						{ 
							len = len * 10 + ch - '0';
							position = position * 10 + ch - '0';
						}
						goto nextch;
					case '$':
						realpos = position;
						len = 0;
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
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].numvalue = value;
						fmtpar[fmtpos].base = 10;
						fmtpar[fmtpos].dosign = 0;
						fmtpar[fmtpos].ljust = ljust;
						fmtpar[fmtpos].len = len;
						fmtpar[fmtpos].zpad = zpad;
						fmtpar[fmtpos].func = FMTNUM;
						fmtpar[fmtpos].realpos = realpos?realpos:fmtpos;
						fmtpos++;
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
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].numvalue = value;
						fmtpar[fmtpos].base = 8;
						fmtpar[fmtpos].dosign = 0;
						fmtpar[fmtpos].ljust = ljust;
						fmtpar[fmtpos].len = len;
						fmtpar[fmtpos].zpad = zpad;
						fmtpar[fmtpos].func = FMTNUM;
						fmtpar[fmtpos].realpos = realpos?realpos:fmtpos;
						fmtpos++;
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
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].numvalue = value;
						fmtpar[fmtpos].base = 10;
						fmtpar[fmtpos].dosign = 1;
						fmtpar[fmtpos].ljust = ljust;
						fmtpar[fmtpos].len = len;
						fmtpar[fmtpos].zpad = zpad;
						fmtpar[fmtpos].func = FMTNUM;
						fmtpar[fmtpos].realpos = realpos?realpos:fmtpos;
						fmtpos++;
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
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].numvalue = value;
						fmtpar[fmtpos].base = 16;
						fmtpar[fmtpos].dosign = 0;
						fmtpar[fmtpos].ljust = ljust;
						fmtpar[fmtpos].len = len;
						fmtpar[fmtpos].zpad = zpad;
						fmtpar[fmtpos].func = FMTNUM;
						fmtpar[fmtpos].realpos = realpos?realpos:fmtpos;
						fmtpos++;
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
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].numvalue = value;
						fmtpar[fmtpos].base = -16;
						fmtpar[fmtpos].dosign = 1;
						fmtpar[fmtpos].ljust = ljust;
						fmtpar[fmtpos].len = len;
						fmtpar[fmtpos].zpad = zpad;
						fmtpar[fmtpos].func = FMTNUM;
						fmtpar[fmtpos].realpos = realpos?realpos:fmtpos;
						fmtpos++;
						break;
					case 's':
						strvalue = va_arg(args, char *);
						if (maxwidth > 0 || !pointflag)
						{
							if (pointflag && len > maxwidth)
								len = maxwidth; /* Adjust padding */
							fmtpar[fmtpos].fmtbegin = fmtbegin;
							fmtpar[fmtpos].fmtend = format;
							fmtpar[fmtpos].value = strvalue;
							fmtpar[fmtpos].ljust = ljust;
							fmtpar[fmtpos].len = len;
							fmtpar[fmtpos].zpad = zpad;
							fmtpar[fmtpos].maxwidth = maxwidth;
							fmtpar[fmtpos].func = FMTSTR;
							fmtpar[fmtpos].realpos = realpos?realpos:fmtpos;
							fmtpos++;
						}
						break;
					case 'c':
						ch = va_arg(args, int);
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].charvalue = ch;
						fmtpar[fmtpos].func = FMTCHAR;
						fmtpar[fmtpos].realpos = realpos?realpos:fmtpos;
						fmtpos++;
						break;
					case 'e':
					case 'E':
					case 'f':
					case 'g':
					case 'G':
						fvalue = va_arg(args, double);
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].fvalue = fvalue;
						fmtpar[fmtpos].type = ch;
						fmtpar[fmtpos].ljust = ljust;
						fmtpar[fmtpos].len = len;
						fmtpar[fmtpos].maxwidth = maxwidth;
						fmtpar[fmtpos].pointflag = pointflag;
						fmtpar[fmtpos].func = FMTFLOAT;
						fmtpar[fmtpos].realpos = realpos?realpos:fmtpos;
						fmtpos++;
						break;
					case '%':
						break;
					default:
						dostr("???????", 0, end);
				}
				break;
			default:
				dopr_outch(ch, end);
				break;
		}
	}
performpr:
	/* shuffle pointers */
	for(i = 1; i < fmtpos; i++)
	{
		fmtparptr[i] = &fmtpar[fmtpar[i].realpos];
	}
	output = buffer;
	format = format_save;
	while ((ch = *format++))
	{
		for(i = 1; i < fmtpos; i++)
		{
			if(ch == '%' && *format == '%')
			{
				format++;
				continue;
			}
			if(fmtpar[i].fmtbegin == format - 1)
			{
				switch(fmtparptr[i]->func){
				case FMTSTR:
					fmtstr(fmtparptr[i]->value, fmtparptr[i]->ljust,
						fmtparptr[i]->len, fmtparptr[i]->zpad,
						fmtparptr[i]->maxwidth, end);
					break;
				case FMTNUM:
					fmtnum(fmtparptr[i]->numvalue, fmtparptr[i]->base,
						fmtparptr[i]->dosign, fmtparptr[i]->ljust,
						fmtparptr[i]->len, fmtparptr[i]->zpad, end);
					break;
				case FMTFLOAT:
					fmtfloat(fmtparptr[i]->fvalue, fmtparptr[i]->type,
						fmtparptr[i]->ljust, fmtparptr[i]->len,
						fmtparptr[i]->precision, fmtparptr[i]->pointflag,
						end);
					break;
				case FMTCHAR:
					dopr_outch(fmtparptr[i]->charvalue, end);
					break;
				}
				format = fmtpar[i].fmtend;
				goto nochar;
			}
		}
		dopr_outch(ch, end);
nochar:
	/* nothing */
	; /* semicolon required because a goto has to be attached to a statement */
	}
	*output = '\0';
}

static void
fmtstr(char *value, int ljust, int len, int zpad, int maxwidth, char *end)
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
		dopr_outch(' ', end);
		--padlen;
	}
	dostr(value, maxwidth, end);
	while (padlen < 0)
	{
		dopr_outch(' ', end);
		++padlen;
	}
}

static void
fmtnum(long_long value, int base, int dosign, int ljust, int len, int zpad, char *end)
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
			dopr_outch(signvalue, end);
			--padlen;
			signvalue = 0;
		}
		while (padlen > 0)
		{
			dopr_outch(zpad, end);
			--padlen;
		}
	}
	while (padlen > 0)
	{
		dopr_outch(' ', end);
		--padlen;
	}
	if (signvalue)
		dopr_outch(signvalue, end);
	while (place > 0)
		dopr_outch(convert[--place], end);
	while (padlen < 0)
	{
		dopr_outch(' ', end);
		++padlen;
	}
}

static void
fmtfloat(double value, char type, int ljust, int len, int precision, int pointflag, char *end)
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
		dopr_outch(' ', end);
		--padlen;
	}
	dostr(convert, 0, end);
	while (padlen < 0)
	{
		dopr_outch(' ', end);
		++padlen;
	}
}

static void
dostr(char *str, int cut, char *end)
{
	if (cut)
	{
		while (*str && cut-- > 0)
			dopr_outch(*str++, end);
	}
	else
	{
		while (*str)
			dopr_outch(*str++, end);
	}
}

static void
dopr_outch(int c, char *end)
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
}
