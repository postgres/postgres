/*
 * stringinfo.c--
 *	  These are routines that can be used to write informations to a string,
 *	  without having to worry about string lengths, space allocation etc.
 *	  Ideally the interface should look like the file i/o interface,
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *	  $Id: stringinfo.c,v 1.13 1998/12/14 08:11:04 scrappy Exp $
 */

#include <stdio.h>
#include <string.h>

#include <stdarg.h>

#include <postgres.h>

#include <nodes/pg_list.h>
#include <lib/stringinfo.h>

/*
 * makeStringInfo
 *
 * Create a StringInfoData & return a pointer to it.
 *
 */
StringInfo
makeStringInfo()
{
	StringInfo	res;
	int			size;

	res = (StringInfo) palloc(sizeof(StringInfoData));
	if (res == NULL)
		elog(ERROR, "makeStringInfo: Out of memory!");

	size = 256;					/* initial default size */
	res->data = palloc(size);
	if (res->data == NULL)
	{
		elog(ERROR,
		   "makeStringInfo: Out of memory! (%d bytes requested)", size);
	}
	res->maxlen = size;
	res->len = 0;
	/* Make sure the string is empty initially. */
	res->data[0] = '\0';

	return res;
}

/*
 * appendStringInfo
 *
 * append to the current 'StringInfo' a new string.
 * If there is not enough space in the current 'data', then reallocate
 * some more...
 *
 * NOTE: if we reallocate space, we pfree the old one!
 *
 */
void
appendStringInfo(StringInfo str, const char *fmt,...)
{
	int		buflen,
				newlen,
				needed;
	char	*s,
				buffer[512];

  va_list args;
	va_start(args, fmt);
  buflen = vsnprintf(buffer, 512, fmt, args);
  va_end(args);

	Assert(str != NULL);
	if (buflen == 0)
		strcpy(buffer, "<>");

	/*
	 * do we have enough space to append the new string? (don't forget to
	 * count the null string terminating char!) If no, then reallocate
	 * some more.
	 */
	needed = str->len + buflen + 1;
	if (needed > str->maxlen)
	{

		/*
		 * how much more space to allocate ? Let's say double the current
		 * space... However we must check if this is enough!
		 */
		newlen = 2 * str->maxlen;
		while (needed > newlen)
			newlen = 2 * newlen;

		/*
		 * allocate enough space.
		 */
		s = palloc(newlen);
		if (s == NULL)
		{
			elog(ERROR,
				 "appendStringInfo: Out of memory (%d bytes requested)", newlen);
		}
		/*
		 * transfer the data.  strcpy() would work, but is probably a tad
		 * slower than memcpy(), and since we know the string length...
		 */
		memcpy(s, str->data, str->len + 1);
		pfree(str->data);
		str->maxlen = newlen;
		str->data = s;
	}

	/*
	 * OK, we have enough space now, append 'buffer' at the end of the
	 * string & update the string length. NOTE: strcat() would work,
	 * but is certainly slower than just memcpy'ing the data to the right
	 * place.
	 */
	memcpy(str->data + str->len, buffer, buflen + 1);
	str->len += buflen;
}
