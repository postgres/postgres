/*-------------------------------------------------------------------------
 *
 * stringinfo.c
 *
 * StringInfo provides an indefinitely-extensible string data type.
 * It can be used to buffer either ordinary C strings (null-terminated text)
 * or arbitrary binary data.  All storage is allocated with palloc().
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *	  $Id: stringinfo.c,v 1.15 1999/04/25 03:19:25 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <string.h>

#include "postgres.h"
#include "lib/stringinfo.h"

/*
 * makeStringInfo
 *
 * Create an empty 'StringInfoData' & return a pointer to it.
 */
StringInfo
makeStringInfo(void)
{
	StringInfo	res;

	res = (StringInfo) palloc(sizeof(StringInfoData));
	if (res == NULL)
		elog(ERROR, "makeStringInfo: Out of memory");

	initStringInfo(res);

	return res;
}

/*
 * initStringInfo
 *
 * Initialize a StringInfoData struct (with previously undefined contents)
 * to describe an empty string.
 */
void
initStringInfo(StringInfo str)
{
	int			size = 256;		/* initial default buffer size */

	str->data = palloc(size);
	if (str->data == NULL)
		elog(ERROR,
			 "initStringInfo: Out of memory (%d bytes requested)", size);
	str->maxlen = size;
	str->len = 0;
	str->data[0] = '\0';
}

/*
 * enlargeStringInfo
 *
 * Internal routine: make sure there is enough space for 'needed' more bytes
 * ('needed' does not include the terminating null).
 */
static void
enlargeStringInfo(StringInfo str, int needed)
{
	int		newlen;
	char   *newdata;

	needed += str->len + 1;		/* total space required now */
	if (needed <= str->maxlen)
		return;					/* got enough space already */

	/*
	 * We don't want to allocate just a little more space with each append;
	 * for efficiency, double the buffer size each time it overflows.
	 * Actually, we might need to more than double it if 'needed' is big...
	 */
	newlen = 2 * str->maxlen;
	while (needed > newlen)
		newlen = 2 * newlen;

	newdata = palloc(newlen);
	if (newdata == NULL)
		elog(ERROR,
			 "enlargeStringInfo: Out of memory (%d bytes requested)", newlen);

	/* OK, transfer data into new buffer, and release old buffer */
	memcpy(newdata, str->data, str->len + 1);
	pfree(str->data);
	str->data = newdata;
	str->maxlen = newlen;
}

/*
 * appendStringInfo
 *
 * Format text data under the control of fmt (an sprintf-like format string)
 * and append it to whatever is already in str.  More space is allocated
 * to str if necessary.  This is sort of like a combination of sprintf and
 * strcat.
 *
 * CAUTION: the current implementation has a 1K limit on the amount of text
 * generated in a single call (not on the total string length).
 */
void
appendStringInfo(StringInfo str, const char *fmt, ...)
{
	va_list	args;
	char	buffer[1024];
	int		buflen;

	Assert(str != NULL);

	va_start(args, fmt);
	buflen = vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	/* Make more room if needed */
	enlargeStringInfo(str, buflen);

	/* OK, append the data, including the trailing null */
	memcpy(str->data + str->len, buffer, buflen + 1);
	str->len += buflen;
}

/*------------------------
 * appendStringInfoChar
 * Append a single byte to str.
 * Like appendStringInfo(str, "%c", ch) but much faster.
 */
void
appendStringInfoChar(StringInfo str, char ch)
{
	Assert(str != NULL);

	/* Make more room if needed */
	enlargeStringInfo(str, 1);

	/* OK, append the character */
	str->data[str->len] = ch;
	str->len++;
	str->data[str->len] = '\0';
}

/*
 * appendBinaryStringInfo
 *
 * Append arbitrary binary data to a StringInfo, allocating more space
 * if necessary.
 */
void
appendBinaryStringInfo(StringInfo str, const char *data, int datalen)
{
	Assert(str != NULL);

	/* Make more room if needed */
	enlargeStringInfo(str, datalen);

	/* OK, append the data */
	memcpy(str->data + str->len, data, datalen);
	str->len += datalen;

	/* Keep a trailing null in place, even though it's probably useless
	 * for binary data...
	 */
	str->data[str->len] = '\0';
}
