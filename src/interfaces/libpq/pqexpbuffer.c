/*-------------------------------------------------------------------------
 *
 * pqexpbuffer.c
 *
 * PQExpBuffer provides an indefinitely-extensible string data type.
 * It can be used to buffer either ordinary C strings (null-terminated text)
 * or arbitrary binary data.  All storage is allocated with malloc().
 *
 * This module is essentially the same as the backend's StringInfo data type,
 * but it is intended for use in frontend libpq and client applications.
 * Thus, it does not rely on palloc(), elog(), nor vsnprintf().
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/interfaces/libpq/pqexpbuffer.c,v 1.1 1999/08/31 01:37:37 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "pqexpbuffer.h"

/*
 * createPQExpBuffer
 *
 * Create an empty 'PQExpBufferData' & return a pointer to it.
 */
PQExpBuffer
createPQExpBuffer(void)
{
	PQExpBuffer	res;

	res = (PQExpBuffer) malloc(sizeof(PQExpBufferData));
	if (res != NULL)
		initPQExpBuffer(res);

	return res;
}

/*
 * initPQExpBuffer
 *
 * Initialize a PQExpBufferData struct (with previously undefined contents)
 * to describe an empty string.
 */
void
initPQExpBuffer(PQExpBuffer str)
{
	str->data = (char *) malloc(INITIAL_EXPBUFFER_SIZE);
	if (str->data == NULL)
	{
		str->maxlen = 0;
		str->len = 0;
	}
	else
	{
		str->maxlen = INITIAL_EXPBUFFER_SIZE;
		str->len = 0;
		str->data[0] = '\0';
	}
}

/*------------------------
 * destroyPQExpBuffer(str);
 *		free()s both the data buffer and the PQExpBufferData.
 *		This is the inverse of createPQExpBuffer().
 */
void
destroyPQExpBuffer(PQExpBuffer str)
{
	if (str)
	{
		termPQExpBuffer(str);
		free(str);
	}
}

/*------------------------
 * termPQExpBuffer(str)
 *		free()s the data buffer but not the PQExpBufferData itself.
 *		This is the inverse of initPQExpBuffer().
 */
void
termPQExpBuffer(PQExpBuffer str)
{
	if (str->data)
	{
		free(str->data);
		str->data = NULL;
	}
}

/*------------------------
 * resetPQExpBuffer
 *		Reset a PQExpBuffer to empty
 */
void
resetPQExpBuffer(PQExpBuffer str)
{
	if (str)
	{
		str->len = 0;
		if (str->data)
			str->data[0] = '\0';
	}
}

/*------------------------
 * enlargePQExpBuffer
 * Make sure there is enough space for 'needed' more bytes in the buffer
 * ('needed' does not include the terminating null).
 *
 * Returns 1 if OK, 0 if failed to enlarge buffer.
 */
int
enlargePQExpBuffer(PQExpBuffer str, int needed)
{
	int			newlen;
	char	   *newdata;

	needed += str->len + 1;		/* total space required now */
	if (needed <= str->maxlen)
		return 1;				/* got enough space already */

	/*
	 * We don't want to allocate just a little more space with each
	 * append; for efficiency, double the buffer size each time it
	 * overflows. Actually, we might need to more than double it if
	 * 'needed' is big...
	 */
	newlen = str->maxlen ? (2 * str->maxlen) : 64;
	while (needed > newlen)
		newlen = 2 * newlen;

	newdata = (char *) realloc(str->data, newlen);
	if (newdata != NULL)
	{
		str->data = newdata;
		str->maxlen = newlen;
		return 1;
	}
	return 0;
}

/*------------------------
 * printfPQExpBuffer
 * Format text data under the control of fmt (an sprintf-like format string)
 * and insert it into str.  More space is allocated to str if necessary.
 * This is a convenience routine that does the same thing as
 * resetPQExpBuffer() followed by appendPQExpBuffer().
 *
 * CAUTION: the frontend version of this routine WILL FAIL if the result of
 * the sprintf formatting operation exceeds 1KB of data (but the size of the
 * pre-existing string in the buffer doesn't matter).  We could make it
 * support larger strings, but that requires vsnprintf() which is not
 * universally available.  Currently there is no need for long strings to be
 * formatted in the frontend.  We could support it, if necessary, by
 * conditionally including a vsnprintf emulation.
 */
void
printfPQExpBuffer(PQExpBuffer str, const char *fmt,...)
{
	va_list		args;
	char		buffer[1024];

	va_start(args, fmt);
	vsprintf(buffer, fmt, args);
	va_end(args);

	resetPQExpBuffer(str);
	appendPQExpBufferStr(str, buffer);
}

/*------------------------
 * appendPQExpBuffer
 *
 * Format text data under the control of fmt (an sprintf-like format string)
 * and append it to whatever is already in str.  More space is allocated
 * to str if necessary.  This is sort of like a combination of sprintf and
 * strcat.
 *
 * CAUTION: the frontend version of this routine WILL FAIL if the result of
 * the sprintf formatting operation exceeds 1KB of data (but the size of the
 * pre-existing string in the buffer doesn't matter).  We could make it
 * support larger strings, but that requires vsnprintf() which is not
 * universally available.  Currently there is no need for long strings to be
 * formatted in the frontend.  We could support it, if necessary, by
 * conditionally including a vsnprintf emulation.
 */
void
appendPQExpBuffer(PQExpBuffer str, const char *fmt,...)
{
	va_list		args;
	char		buffer[1024];

	va_start(args, fmt);
	vsprintf(buffer, fmt, args);
	va_end(args);

	appendPQExpBufferStr(str, buffer);
}

/*------------------------
 * appendPQExpBufferStr
 * Append the given string to a PQExpBuffer, allocating more space
 * if necessary.
 */
void
appendPQExpBufferStr(PQExpBuffer str, const char *data)
{
	appendBinaryPQExpBuffer(str, data, strlen(data));
}

/*------------------------
 * appendPQExpBufferChar
 * Append a single byte to str.
 * Like appendPQExpBuffer(str, "%c", ch) but much faster.
 */
void
appendPQExpBufferChar(PQExpBuffer str, char ch)
{
	/* Make more room if needed */
	if (! enlargePQExpBuffer(str, 1))
		return;

	/* OK, append the character */
	str->data[str->len] = ch;
	str->len++;
	str->data[str->len] = '\0';
}

/*
 * appendBinaryPQExpBuffer
 *
 * Append arbitrary binary data to a PQExpBuffer, allocating more space
 * if necessary.
 */
void
appendBinaryPQExpBuffer(PQExpBuffer str, const char *data, int datalen)
{
	/* Make more room if needed */
	if (! enlargePQExpBuffer(str, datalen))
		return;

	/* OK, append the data */
	memcpy(str->data + str->len, data, datalen);
	str->len += datalen;

	/*
	 * Keep a trailing null in place, even though it's probably useless
	 * for binary data...
	 */
	str->data[str->len] = '\0';
}
