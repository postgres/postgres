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

#include "postgres.h"

#ifndef WIN32
#include <sys/ioctl.h>
#endif
#include <sys/param.h>


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

/*static char _id[] = "$PostgreSQL: pgsql/src/port/snprintf.c,v 1.26 2005/03/20 13:54:53 momjian Exp $";*/

static void dopr(char *buffer, const char *format, va_list args, char *end);

/* Prevent recursion */
#undef	vsnprintf
#undef	snprintf
#undef	sprintf
#undef	fprintf
#undef	printf

int
pg_vsnprintf(char *str, size_t count, const char *fmt, va_list args)
{
	char	   *end;

	str[0] = '\0';
	end = str + count - 1;
	dopr(str, fmt, args, end);
	if (count > 0)
		end[0] = '\0';
	return strlen(str);
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

int
pg_sprintf(char *str, const char *fmt,...)
{
	int			len;
	va_list		args;
	char		buffer[4096];

	va_start(args, fmt);
	len = pg_vsnprintf(buffer, (size_t) 4096, fmt, args);
	va_end(args);
	/* limit output to string */
	StrNCpy(str, buffer, (len + 1 < 4096) ? len + 1 : 4096);
	return len;
}

int
pg_fprintf(FILE *stream, const char *fmt,...)
{
	int			len;
	va_list		args;
	char		buffer[4096];
	char	   *p;

	va_start(args, fmt);
	len = pg_vsnprintf(buffer, (size_t) 4096, fmt, args);
	va_end(args);
	for (p = buffer; *p; p++)
		putc(*p, stream);
	return len;
}

int
pg_printf(const char *fmt,...)
{
	int			len;
	va_list		args;
	char		buffer[4096];
	char	   *p;

	va_start(args, fmt);
	len = pg_vsnprintf(buffer, (size_t) 4096, fmt, args);
	va_end(args);
	
	for (p = buffer; *p; p++)
		putchar(*p);
	return len;
}

static int adjust_sign(int is_negative, int forcesign, int *signvalue);
static void adjust_padlen(int minlen, int vallen, int leftjust, int *padlen);
static void leading_pad(int zpad, int *signvalue, int *padlen, char *end,
				 char **output);
static void trailing_pad(int *padlen, char *end, char **output);

static void fmtstr(char *value, int leftjust, int minlen, int maxwidth,
	   char *end, char **output);
static void fmtint(int64 value, int base, int dosign, int forcesign,
	   int leftjust, int minlen, int zpad, char *end, char **output);
static void fmtfloat(double value, char type, int forcesign,
	   int leftjust, int minlen, int zpad, int precision, int pointflag, char *end,
	   char **output);
static void dostr(char *str, int cut, char *end, char **output);
static void dopr_outch(int c, char *end, char **output);

#define FMTSTR		1
#define FMTNUM		2
#define FMTNUM_U	3
#define FMTFLOAT	4
#define FMTCHAR		5
#define FMTWIDTH	6
#define FMTLEN		7

/*
 * dopr(): poor man's version of doprintf
 */

static void
dopr(char *buffer, const char *format, va_list args, char *end)
{
	int			ch;
	int			longlongflag;
	int			longflag;
	int			pointflag;
	int			maxwidth;
	int			leftjust;
	int			minlen;
	int			zpad;
	int			forcesign;
	int			i;
	const char *format_save;
	const char *fmtbegin;
	int			fmtpos = 1;
	int			realpos = 0;
	int			precision;
	int			position;
	char	   *output;
	int			percents = 1;
	const char *p;
	struct fmtpar
	{
		const char *fmtbegin;
		const char *fmtend;
		void	   *value;
		int64		numvalue;
		double		fvalue;
		int			charvalue;
		int			leftjust;
		int			minlen;
		int			zpad;
		int			maxwidth;
		int			base;
		int			dosign;
		int			forcesign;
		char		type;
		int			precision;
		int			pointflag;
		char		func;
		int			realpos;
		int			longflag;
		int			longlongflag;
	}		   *fmtpar, **fmtparptr;

	/* Create enough structures to hold all arguments */
	for (p = format; *p != '\0'; p++)
		if (*p == '%')			/* counts %% as two, so overcounts */
			percents++;

	/* Need to use malloc() because memory system might not be started yet. */
	if ((fmtpar = malloc(sizeof(struct fmtpar) * percents)) == NULL)
	{
		fprintf(stderr, _("out of memory\n"));
		exit(1);
	}
	if ((fmtparptr = malloc(sizeof(struct fmtpar *) * percents)) == NULL)
	{
		fprintf(stderr, _("out of memory\n"));
		exit(1);
	}

	format_save = format;

	output = buffer;
	while ((ch = *format++))
	{
		switch (ch)
		{
			case '%':
				leftjust = minlen = zpad = forcesign = maxwidth = 0;
				longflag = longlongflag = pointflag = 0;
				fmtbegin = format - 1;
				realpos = 0;
				position = precision = 0;
		nextch:
				ch = *format++;
				switch (ch)
				{
					case '\0':
						goto performpr;
					case '-':
						leftjust = 1;
						goto nextch;
					case '+':
						forcesign = 1;
						goto nextch;
					case '0':	/* set zero padding if minlen not set */
						if (minlen == 0 && !pointflag)
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
						if (!pointflag)
						{
							minlen = minlen * 10 + ch - '0';
							position = position * 10 + ch - '0';
						}
						else
						{
							maxwidth = maxwidth * 10 + ch - '0';
							precision = precision * 10 + ch - '0';
						}
						goto nextch;
					case '$':
						realpos = position;
						minlen = 0;
						goto nextch;
					case '*':
						MemSet(&fmtpar[fmtpos], 0, sizeof(fmtpar[fmtpos]));
						if (!pointflag)
							fmtpar[fmtpos].func = FMTLEN;
						else
							fmtpar[fmtpos].func = FMTWIDTH;
						fmtpar[fmtpos].realpos = realpos ? realpos : fmtpos;
						fmtpos++;
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
					case 'h':
						/* ignore */
						goto nextch;
#ifdef NOT_USED

						/*
						 * We might export this to client apps so we should
						 * support 'qd' and 'I64d'(MinGW) also in case the
						 * native version does.
						 */
					case 'q':
						longlongflag = 1;
						longflag = 1;
						goto nextch;
					case 'I':
						if (*format == '6' && *(format + 1) == '4')
						{
							format += 2;
							longlongflag = 1;
							longflag = 1;
							goto nextch;
						}
						break;
#endif
					case 'u':
					case 'U':
						fmtpar[fmtpos].longflag = longflag;
						fmtpar[fmtpos].longlongflag = longlongflag;
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].base = 10;
						fmtpar[fmtpos].dosign = 0;
						fmtpar[fmtpos].forcesign = forcesign;
						fmtpar[fmtpos].leftjust = leftjust;
						fmtpar[fmtpos].minlen = minlen;
						fmtpar[fmtpos].zpad = zpad;
						fmtpar[fmtpos].func = FMTNUM_U;
						fmtpar[fmtpos].realpos = realpos ? realpos : fmtpos;
						fmtpos++;
						break;
					case 'o':
					case 'O':
						fmtpar[fmtpos].longflag = longflag;
						fmtpar[fmtpos].longlongflag = longlongflag;
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].base = 8;
						fmtpar[fmtpos].dosign = 0;
						fmtpar[fmtpos].forcesign = forcesign;
						fmtpar[fmtpos].leftjust = leftjust;
						fmtpar[fmtpos].minlen = minlen;
						fmtpar[fmtpos].zpad = zpad;
						fmtpar[fmtpos].func = FMTNUM_U;
						fmtpar[fmtpos].realpos = realpos ? realpos : fmtpos;
						fmtpos++;
						break;
					case 'd':
					case 'D':
						fmtpar[fmtpos].longflag = longflag;
						fmtpar[fmtpos].longlongflag = longlongflag;
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].base = 10;
						fmtpar[fmtpos].dosign = 1;
						fmtpar[fmtpos].forcesign = forcesign;
						fmtpar[fmtpos].leftjust = leftjust;
						fmtpar[fmtpos].minlen = minlen;
						fmtpar[fmtpos].zpad = zpad;
						fmtpar[fmtpos].func = FMTNUM;
						fmtpar[fmtpos].realpos = realpos ? realpos : fmtpos;
						fmtpos++;
						break;
					case 'x':
						fmtpar[fmtpos].longflag = longflag;
						fmtpar[fmtpos].longlongflag = longlongflag;
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].base = 16;
						fmtpar[fmtpos].dosign = 0;
						fmtpar[fmtpos].forcesign = forcesign;
						fmtpar[fmtpos].leftjust = leftjust;
						fmtpar[fmtpos].minlen = minlen;
						fmtpar[fmtpos].zpad = zpad;
						fmtpar[fmtpos].func = FMTNUM_U;
						fmtpar[fmtpos].realpos = realpos ? realpos : fmtpos;
						fmtpos++;
						break;
					case 'X':
						fmtpar[fmtpos].longflag = longflag;
						fmtpar[fmtpos].longlongflag = longlongflag;
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].base = -16;
						fmtpar[fmtpos].dosign = 1;
						fmtpar[fmtpos].forcesign = forcesign;
						fmtpar[fmtpos].leftjust = leftjust;
						fmtpar[fmtpos].minlen = minlen;
						fmtpar[fmtpos].zpad = zpad;
						fmtpar[fmtpos].func = FMTNUM_U;
						fmtpar[fmtpos].realpos = realpos ? realpos : fmtpos;
						fmtpos++;
						break;
					case 's':
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].leftjust = leftjust;
						fmtpar[fmtpos].minlen = minlen;
						fmtpar[fmtpos].zpad = zpad;
						fmtpar[fmtpos].maxwidth = maxwidth;
						fmtpar[fmtpos].func = FMTSTR;
						fmtpar[fmtpos].realpos = realpos ? realpos : fmtpos;
						fmtpos++;
						break;
					case 'c':
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].func = FMTCHAR;
						fmtpar[fmtpos].realpos = realpos ? realpos : fmtpos;
						fmtpos++;
						break;
					case 'e':
					case 'E':
					case 'f':
					case 'g':
					case 'G':
						fmtpar[fmtpos].fmtbegin = fmtbegin;
						fmtpar[fmtpos].fmtend = format;
						fmtpar[fmtpos].type = ch;
						fmtpar[fmtpos].forcesign = forcesign;
						fmtpar[fmtpos].leftjust = leftjust;
						fmtpar[fmtpos].minlen = minlen;
						fmtpar[fmtpos].zpad = zpad;
						fmtpar[fmtpos].precision = precision;
						fmtpar[fmtpos].pointflag = pointflag;
						fmtpar[fmtpos].func = FMTFLOAT;
						fmtpar[fmtpos].realpos = realpos ? realpos : fmtpos;
						fmtpos++;
						break;
					case '%':
						break;
					default:
						dostr("???????", 0, end, &output);
				}
				break;
			default:
				dopr_outch(ch, end, &output);
				break;
		}
	}

performpr:
	/* reorder pointers */
	for (i = 1; i < fmtpos; i++)
		fmtparptr[i] = &fmtpar[fmtpar[i].realpos];

	/* assign values */
	for (i = 1; i < fmtpos; i++)
	{
		switch (fmtparptr[i]->func)
		{
			case FMTSTR:
				fmtparptr[i]->value = va_arg(args, char *);
				break;
			case FMTNUM:
				if (fmtparptr[i]->longflag)
				{
					if (fmtparptr[i]->longlongflag)
						fmtparptr[i]->numvalue = va_arg(args, int64);
					else
						fmtparptr[i]->numvalue = va_arg(args, long);
				}
				else
					fmtparptr[i]->numvalue = va_arg(args, int);
				break;
			case FMTNUM_U:
				if (fmtparptr[i]->longflag)
				{
					if (fmtparptr[i]->longlongflag)
						fmtparptr[i]->numvalue = va_arg(args, uint64);
					else
						fmtparptr[i]->numvalue = va_arg(args, unsigned long);
				}
				else
					fmtparptr[i]->numvalue = va_arg(args, unsigned int);
				break;
			case FMTFLOAT:
				fmtparptr[i]->fvalue = va_arg(args, double);
				break;
			case FMTCHAR:
				fmtparptr[i]->charvalue = va_arg(args, int);
				break;
			case FMTLEN:
				{
					int minlen = va_arg(args, int);
					int leftjust = 0;

					if (minlen < 0)
					{
						minlen = -minlen;
						leftjust = 1;
					}
					if (i + 1 < fmtpos && fmtparptr[i + 1]->func != FMTWIDTH)
					{
						fmtparptr[i + 1]->minlen = minlen;
						fmtparptr[i + 1]->leftjust |= leftjust;
					}
					/* For "%*.*f", use the second arg */
					if (i + 2 < fmtpos && fmtparptr[i + 1]->func == FMTWIDTH)
					{
						fmtparptr[i + 2]->minlen = minlen;
						fmtparptr[i + 2]->leftjust |= leftjust;
					}
				}
				break;
			case FMTWIDTH:
				if (i + 1 < fmtpos)
					fmtparptr[i + 1]->maxwidth = fmtparptr[i + 1]->precision =
						va_arg(args, int);
				break;
		}
	}

	/* do the output */
	output = buffer;
	format = format_save;
	while ((ch = *format++))
	{
		for (i = 1; i < fmtpos; i++)
		{
			if (ch == '%' && *format == '%')
			{
				format++;
				continue;
			}
			if (fmtpar[i].fmtbegin == format - 1)
			{
				switch (fmtparptr[i]->func)
				{
					case FMTSTR:
						fmtstr(fmtparptr[i]->value, fmtparptr[i]->leftjust,
							   fmtparptr[i]->minlen, fmtparptr[i]->maxwidth,
							   end, &output);
						break;
					case FMTNUM:
					case FMTNUM_U:
						fmtint(fmtparptr[i]->numvalue, fmtparptr[i]->base,
							   fmtparptr[i]->dosign, fmtparptr[i]->forcesign,
							   fmtparptr[i]->leftjust, fmtparptr[i]->minlen,
							   fmtparptr[i]->zpad, end, &output);
						break;
					case FMTFLOAT:
						fmtfloat(fmtparptr[i]->fvalue, fmtparptr[i]->type,
							   fmtparptr[i]->forcesign, fmtparptr[i]->leftjust,
							   fmtparptr[i]->minlen, fmtparptr[i]->zpad,
							   fmtparptr[i]->precision, fmtparptr[i]->pointflag,
							   end, &output);
						break;
					case FMTCHAR:
						dopr_outch(fmtparptr[i]->charvalue, end, &output);
						break;
				}
				format = fmtpar[i].fmtend;
				goto nochar;
			}
		}
		dopr_outch(ch, end, &output);
nochar:
		/* nothing */
		;						/* semicolon required because a goto has to be
								 * attached to a statement */
	}
	*output = '\0';

	free(fmtpar);
	free(fmtparptr);
}

static void
fmtstr(char *value, int leftjust, int minlen, int maxwidth, char *end,
	   char **output)
{
	int			padlen,
				vallen;			/* amount to pad */

	if (value == NULL)
		value = "<NULL>";

	vallen = strlen(value);
	if (maxwidth && vallen > maxwidth)
		vallen = maxwidth;

	adjust_padlen(minlen, vallen, leftjust, &padlen);

	while (padlen > 0)
	{
		dopr_outch(' ', end, output);
		--padlen;
	}
	dostr(value, maxwidth, end, output);

	trailing_pad(&padlen, end, output);
}

static void
fmtint(int64 value, int base, int dosign, int forcesign, int leftjust,
	   int minlen, int zpad, char *end, char **output)
{
	int			signvalue = 0;
	char		convert[64];
	int			vallen = 0;
	int			padlen = 0;		/* amount to pad */
	int			caps = 0;

	/* Handle +/- and %X (uppercase hex) */
	if (dosign && adjust_sign((value < 0), forcesign, &signvalue))
			value = -value;
	if (base < 0)
	{
		caps = 1;
		base = -base;
	}

	/* make integer string */
	do
	{
		convert[vallen++] = (caps ? "0123456789ABCDEF" : "0123456789abcdef")
			[value % (unsigned) base];
		value = (value / (unsigned) base);
	} while (value);
	convert[vallen] = 0;

	adjust_padlen(minlen, vallen, leftjust, &padlen);

	leading_pad(zpad, &signvalue, &padlen, end, output);
	
	while (vallen > 0)
		dopr_outch(convert[--vallen], end, output);

	trailing_pad(&padlen, end, output);
}

static void
fmtfloat(double value, char type, int forcesign, int leftjust,
		 int minlen, int zpad, int precision, int pointflag, char *end,
		 char **output)
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

	leading_pad(zpad, &signvalue, &padlen, end, output);

	dostr(convert, 0, end, output);

	trailing_pad(&padlen, end, output);
}

static void
dostr(char *str, int cut, char *end, char **output)
{
	if (cut)
		while (*str && cut-- > 0)
			dopr_outch(*str++, end, output);
	else
		while (*str)
			dopr_outch(*str++, end, output);
}

static void
dopr_outch(int c, char *end, char **output)
{
#ifdef NOT_USED
	if (iscntrl((unsigned char) c) && c != '\n' && c != '\t')
	{
		c = '@' + (c & 0x1F);
		if (end == 0 || *output < end)
			*(*output)++ = '^';
	}
#endif
	if (end == 0 || *output < end)
		*(*output)++ = c;
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
		*padlen = -*padlen;
}


static void
leading_pad(int zpad, int *signvalue, int *padlen, char *end, char **output)
{
	if (*padlen > 0 && zpad)
	{
		if (*signvalue)
		{
			dopr_outch(*signvalue, end, output);
			--*padlen;
			*signvalue = 0;
		}
		while (*padlen > 0)
		{
			dopr_outch(zpad, end, output);
			--*padlen;
		}
	}
	while (*padlen > 0 + (*signvalue != 0))
	{
		dopr_outch(' ', end, output);
		--*padlen;
	}
	if (*signvalue)
	{
		dopr_outch(*signvalue, end, output);
		if (*padlen > 0)
			--*padlen;
		if (padlen < 0)
			++padlen;
	}
}


static void
trailing_pad(int *padlen, char *end, char **output)
{
	while (*padlen < 0)
	{
		dopr_outch(' ', end, output);
		++*padlen;
	}
}

