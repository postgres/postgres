/*
 * string_io.c --
 *
 * This file defines C-like input/output conversion routines for strings.
 *
 * Copyright (c) 1998, Massimo Dal Zotto <dz@cs.unitn.it>
 *
 * This file is distributed under the GNU General Public License
 * either version 2, or (at your option) any later version.
 */

#include <ctype.h>
#include <string.h>

#include "postgres.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/builtins.h"

#include "string_io.h"

/* define this if you want to see iso-8859 characters */
#define ISO8859

#define MIN(x, y)	((x) < (y) ? (x) : (y))
#define VALUE(char)		((char) - '0')
#define DIGIT(val)	((val) + '0')
#define ISOCTAL(c)	(((c) >= '0') && ((c) <= '7'))
#ifndef ISO8859
#define NOTPRINTABLE(c) (!isprint(c))
#else
#define NOTPRINTABLE(c) (!isprint(c) && ((c) < 0xa0))
#endif

/*
 * string_output() --
 *
 * This function takes a pointer to a string data and an optional
 * data size and returns a printable representation of the string
 * translating all escape sequences to C-like \nnn or \c escapes.
 * The function is used by output methods of various string types.
 *
 * Arguments:
 *	data -		input data (can be NULL)
 *	size -		optional size of data. A negative value indicates
 *				that data is a null terminated string.
 *
 * Returns:
 *	a pointer to a new string containing the printable
 *	representation of data.
 */

char *
string_output(char *data, int size)
{
	register unsigned char c,
			   *p,
			   *r,
			   *result;
	register int l,
				len;

	if (data == NULL)
	{
		result = (char *) palloc(2);
		result[0] = '-';
		result[1] = '\0';
		return (result);
	}

	if (size < 0)
		size = strlen(data);

	/* adjust string length for escapes */
	len = size;
	for (p = data, l = size; l > 0; p++, l--)
	{
		switch (*p)
		{
			case '\\':
			case '"':
			case '{':
			case '}':
			case '\b':
			case '\f':
			case '\n':
			case '\r':
			case '\t':
			case '\v':
				len++;
				break;
			default:
				if (NOTPRINTABLE(*p))
					len += 3;
		}
	}
	len++;

	result = (char *) palloc(len);

	for (p = data, r = result, l = size; (l > 0) && (c = *p); p++, l--)
	{
		switch (c)
		{
			case '\\':
			case '"':
			case '{':
			case '}':
				*r++ = '\\';
				*r++ = c;
				break;
			case '\b':
				*r++ = '\\';
				*r++ = 'b';
				break;
			case '\f':
				*r++ = '\\';
				*r++ = 'f';
				break;
			case '\n':
				*r++ = '\\';
				*r++ = 'n';
				break;
			case '\r':
				*r++ = '\\';
				*r++ = 'r';
				break;
			case '\t':
				*r++ = '\\';
				*r++ = 't';
				break;
			case '\v':
				*r++ = '\\';
				*r++ = 'v';
				break;
			default:
				if (NOTPRINTABLE(c))
				{
					*r = '\\';
					r += 3;
					*r-- = DIGIT(c & 07);
					c >>= 3;
					*r-- = DIGIT(c & 07);
					c >>= 3;
					*r = DIGIT(c & 03);
					r += 3;
				}
				else
					*r++ = c;
		}
	}
	*r = '\0';

	return ((char *) result);
}

/*
 * string_input() --
 *
 * This function accepts a C string in input and copies it into a new
 * object allocated with palloc() translating all escape sequences.
 * An optional header can be allocatd before the string, for example
 * to hold the length of a varlena object.
 * This function is not necessary for input from sql commands because
 * the parser already does escape translation, all data input routines
 * receive strings in internal form.
 *
 * Arguments:
 *	str -		input string possibly with escapes
 *	size -		the required size of new data. A value of 0
 *				indicates a variable size string, while a
 *				negative value indicates a variable size string
 *				of size not greater than this absolute value.
 *	hdrsize -	size of an optional header to be allocated before
 *				the data. It must then be filled by the caller.
 *	rtn_size -	an optional pointer to an int variable where the
 *				size of the new string is stored back.
 *
 * Returns:
 *	a pointer to the new string or the header.
 */

char *
string_input(char *str, int size, int hdrsize, int *rtn_size)
{
	register unsigned char *p,
			   *r;
	unsigned char *result;
	int			len;

	if ((str == NULL) || (hdrsize < 0))
		return (char *) NULL;

	/* Compute result size */
	len = strlen(str);
	for (p = str; *p;)
	{
		if (*p++ == '\\')
		{
			if (ISOCTAL(*p))
			{
				if (ISOCTAL(*(p + 1)))
				{
					p++;
					len--;
				}
				if (ISOCTAL(*(p + 1)))
				{
					p++;
					len--;
				}
			}
			if (*p)
				p++;
			len--;
		}
	}

	/* result has variable length */
	if (size == 0)
		size = len + 1;
	else
		/* result has variable length with maximum size */
	if (size < 0)
		size = MIN(len, -size) + 1;

	result = (char *) palloc(hdrsize + size);
	memset(result, 0, hdrsize + size);
	if (rtn_size)
		*rtn_size = size;

	r = result + hdrsize;
	for (p = str; *p;)
	{
		register unsigned char c;

		if ((c = *p++) == '\\')
		{
			switch (c = *p++)
			{
				case '\0':
					p--;
					break;
				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
					c = VALUE(c);
					if (isdigit(*p))
						c = (c << 3) + VALUE(*p++);
					if (isdigit(*p))
						c = (c << 3) + VALUE(*p++);
					*r++ = c;
					break;
				case 'b':
					*r++ = '\b';
					break;
				case 'f':
					*r++ = '\f';
					break;
				case 'n':
					*r++ = '\n';
					break;
				case 'r':
					*r++ = '\r';
					break;
				case 't':
					*r++ = '\t';
					break;
				case 'v':
					*r++ = '\v';
					break;
				default:
					*r++ = c;
			}
		}
		else
			*r++ = c;
	}

	return ((char *) result);
}

char *
c_charout(int32 c)
{
	char		str[2];

	str[0] = (char) c;
	str[1] = '\0';

	return (string_output(str, 1));
}

/*
 * This can be used for SET, bytea, text and unknown data types
 */

char *
c_textout(struct varlena * vlena)
{
	int			len = 0;
	char	   *s = NULL;

	if (vlena)
	{
		len = VARSIZE(vlena) - VARHDRSZ;
		s = VARDATA(vlena);
	}
	return (string_output(s, len));
}

/*
 * This can be used for varchar and bpchar strings
 */

char *
c_varcharout(char *s)
{
	int			len = 0;

	if (s)
	{
		len = *(int32 *) s - 4;
		s += 4;
	}
	return (string_output(s, len));
}

#if 0
struct varlena *
c_textin(char *str)
{
	struct varlena *result;
	int			len;

	if (str == NULL)
		return ((struct varlena *) NULL);

	result = (struct varlena *) string_input(str, 0, VARHDRSZ, &len);
	VARSIZE(result) = len;

	return (result);
}

int32 *
c_charin(char *str)
{
	return (string_input(str, 1, 0, NULL));
}

#endif

/* end of file */

/*
 * Local variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */
