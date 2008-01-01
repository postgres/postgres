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
 * Thus, it does not rely on palloc() nor elog().
 *
 * It does rely on vsnprintf(); if configure finds that libc doesn't provide
 * a usable vsnprintf(), then a copy of our own implementation of it will
 * be linked into libpq.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/interfaces/libpq/pqexpbuffer.c,v 1.24 2008/01/01 19:46:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <limits.h>

#include "pqexpbuffer.h"

#ifdef WIN32
#include "win32.h"
#endif

/*
 * createPQExpBuffer
 *
 * Create an empty 'PQExpBufferData' & return a pointer to it.
 */
PQExpBuffer
createPQExpBuffer(void)
{
	PQExpBuffer res;

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

/*
 * destroyPQExpBuffer(str);
 *
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

/*
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
	/* just for luck, make the buffer validly empty. */
	str->maxlen = 0;
	str->len = 0;
}

/*
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

/*
 * enlargePQExpBuffer
 * Make sure there is enough space for 'needed' more bytes in the buffer
 * ('needed' does not include the terminating null).
 *
 * Returns 1 if OK, 0 if failed to enlarge buffer.
 */
int
enlargePQExpBuffer(PQExpBuffer str, size_t needed)
{
	size_t		newlen;
	char	   *newdata;

	/*
	 * Guard against ridiculous "needed" values, which can occur if we're fed
	 * bogus data.	Without this, we can get an overflow or infinite loop in
	 * the following.
	 */
	if (needed >= ((size_t) INT_MAX - str->len))
		return 0;

	needed += str->len + 1;		/* total space required now */

	/* Because of the above test, we now have needed <= INT_MAX */

	if (needed <= str->maxlen)
		return 1;				/* got enough space already */

	/*
	 * We don't want to allocate just a little more space with each append;
	 * for efficiency, double the buffer size each time it overflows.
	 * Actually, we might need to more than double it if 'needed' is big...
	 */
	newlen = (str->maxlen > 0) ? (2 * str->maxlen) : 64;
	while (needed > newlen)
		newlen = 2 * newlen;

	/*
	 * Clamp to INT_MAX in case we went past it.  Note we are assuming here
	 * that INT_MAX <= UINT_MAX/2, else the above loop could overflow.	We
	 * will still have newlen >= needed.
	 */
	if (newlen > (size_t) INT_MAX)
		newlen = (size_t) INT_MAX;

	newdata = (char *) realloc(str->data, newlen);
	if (newdata != NULL)
	{
		str->data = newdata;
		str->maxlen = newlen;
		return 1;
	}
	return 0;
}

/*
 * printfPQExpBuffer
 * Format text data under the control of fmt (an sprintf-like format string)
 * and insert it into str.	More space is allocated to str if necessary.
 * This is a convenience routine that does the same thing as
 * resetPQExpBuffer() followed by appendPQExpBuffer().
 */
void
printfPQExpBuffer(PQExpBuffer str, const char *fmt,...)
{
	va_list		args;
	size_t		avail;
	int			nprinted;

	resetPQExpBuffer(str);

	for (;;)
	{
		/*
		 * Try to format the given string into the available space; but if
		 * there's hardly any space, don't bother trying, just fall through to
		 * enlarge the buffer first.
		 */
		if (str->maxlen > str->len + 16)
		{
			avail = str->maxlen - str->len - 1;
			va_start(args, fmt);
			nprinted = vsnprintf(str->data + str->len, avail,
								 fmt, args);
			va_end(args);

			/*
			 * Note: some versions of vsnprintf return the number of chars
			 * actually stored, but at least one returns -1 on failure. Be
			 * conservative about believing whether the print worked.
			 */
			if (nprinted >= 0 && nprinted < (int) avail - 1)
			{
				/* Success.  Note nprinted does not include trailing null. */
				str->len += nprinted;
				break;
			}
		}
		/* Double the buffer size and try again. */
		if (!enlargePQExpBuffer(str, str->maxlen))
			return;				/* oops, out of memory */
	}
}

/*
 * appendPQExpBuffer
 *
 * Format text data under the control of fmt (an sprintf-like format string)
 * and append it to whatever is already in str.  More space is allocated
 * to str if necessary.  This is sort of like a combination of sprintf and
 * strcat.
 */
void
appendPQExpBuffer(PQExpBuffer str, const char *fmt,...)
{
	va_list		args;
	size_t		avail;
	int			nprinted;

	for (;;)
	{
		/*
		 * Try to format the given string into the available space; but if
		 * there's hardly any space, don't bother trying, just fall through to
		 * enlarge the buffer first.
		 */
		if (str->maxlen > str->len + 16)
		{
			avail = str->maxlen - str->len - 1;
			va_start(args, fmt);
			nprinted = vsnprintf(str->data + str->len, avail,
								 fmt, args);
			va_end(args);

			/*
			 * Note: some versions of vsnprintf return the number of chars
			 * actually stored, but at least one returns -1 on failure. Be
			 * conservative about believing whether the print worked.
			 */
			if (nprinted >= 0 && nprinted < (int) avail - 1)
			{
				/* Success.  Note nprinted does not include trailing null. */
				str->len += nprinted;
				break;
			}
		}
		/* Double the buffer size and try again. */
		if (!enlargePQExpBuffer(str, str->maxlen))
			return;				/* oops, out of memory */
	}
}

/*
 * appendPQExpBufferStr
 * Append the given string to a PQExpBuffer, allocating more space
 * if necessary.
 */
void
appendPQExpBufferStr(PQExpBuffer str, const char *data)
{
	appendBinaryPQExpBuffer(str, data, strlen(data));
}

/*
 * appendPQExpBufferChar
 * Append a single byte to str.
 * Like appendPQExpBuffer(str, "%c", ch) but much faster.
 */
void
appendPQExpBufferChar(PQExpBuffer str, char ch)
{
	/* Make more room if needed */
	if (!enlargePQExpBuffer(str, 1))
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
appendBinaryPQExpBuffer(PQExpBuffer str, const char *data, size_t datalen)
{
	/* Make more room if needed */
	if (!enlargePQExpBuffer(str, datalen))
		return;

	/* OK, append the data */
	memcpy(str->data + str->len, data, datalen);
	str->len += datalen;

	/*
	 * Keep a trailing null in place, even though it's probably useless for
	 * binary data...
	 */
	str->data[str->len] = '\0';
}
