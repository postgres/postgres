/*
 * This file is in the public domain, so clarified as of
 * 1996-06-05 by Arthur David Olson (arthur_david_olson@nih.gov).
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/timezone/scheck.c,v 1.8 2007/10/26 13:30:10 tgl Exp $
 */

#include "postgres_fe.h"

#include "private.h"


char *
scheck(const char *string, const char *format)
{
	char	   *fbuf;
	const char *fp;
	char	   *tp;
	int			c;
	char	   *result;
	char		dummy;
	static char nada;

	result = &nada;
	if (string == NULL || format == NULL)
		return result;
	fbuf = imalloc((int) (2 * strlen(format) + 4));
	if (fbuf == NULL)
		return result;
	fp = format;
	tp = fbuf;
	while ((*tp++ = c = *fp++) != '\0')
	{
		if (c != '%')
			continue;
		if (*fp == '%')
		{
			*tp++ = *fp++;
			continue;
		}
		*tp++ = '*';
		if (*fp == '*')
			++fp;
		while (is_digit(*fp))
			*tp++ = *fp++;
		if (*fp == 'l' || *fp == 'h')
			*tp++ = *fp++;
		else if (*fp == '[')
			do
				*tp++ = *fp++;
			while (*fp != '\0' && *fp != ']');
		if ((*tp++ = *fp++) == '\0')
			break;
	}
	*(tp - 1) = '%';
	*tp++ = 'c';
	*tp = '\0';
	if (sscanf(string, fbuf, &dummy) != 1)
		result = (char *) format;
	ifree(fbuf);
	return result;
}
