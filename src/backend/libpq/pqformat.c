/*-------------------------------------------------------------------------
 *
 * pqformat.c
 *		Routines for formatting and parsing frontend/backend messages
 *
 * Outgoing messages are built up in a StringInfo buffer (which is expansible)
 * and then sent in a single call to pq_putmessage.  This module provides data
 * formatting/conversion routines that are needed to produce valid messages.
 * Note in particular the distinction between "raw data" and "text"; raw data
 * is message protocol characters and binary values that are not subject to
 * character set conversion, while text is converted by character encoding rules.
 *
 * Incoming messages are read directly off the wire, as it were, but there
 * are still data-conversion tasks to be performed.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$Id: pqformat.c,v 1.26 2003/04/02 00:49:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 * Message assembly and output:
 *		pq_beginmessage - initialize StringInfo buffer
 *		pq_sendbyte		- append a raw byte to a StringInfo buffer
 *		pq_sendint		- append a binary integer to a StringInfo buffer
 *		pq_sendbytes	- append raw data to a StringInfo buffer
 *		pq_sendcountedtext - append a text string (with character set conversion)
 *		pq_sendstring	- append a null-terminated text string (with conversion)
 *		pq_endmessage	- send the completed message to the frontend
 * Note: it is also possible to append data to the StringInfo buffer using
 * the regular StringInfo routines, but this is discouraged since required
 * character set conversion may not occur.
 *
 * Special-case message output:
 *		pq_puttextmessage - generate a character set-converted message in one step
 *
 * Message input:
 *		pq_getint			- get an integer from connection
 *		pq_getstr_bounded	- get a null terminated string from connection
 * pq_getstr_bounded performs character set conversion on the collected
 * string.  Use the raw pqcomm.c routines pq_getstring or pq_getbytes
 * to fetch data without conversion.
 */

#include "postgres.h"

#include <errno.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_ENDIAN_H
#include <endian.h>
#endif

#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"


/* --------------------------------
 *		pq_sendbyte		- append a raw byte to a StringInfo buffer
 * --------------------------------
 */
void
pq_sendbyte(StringInfo buf, int byt)
{
	appendStringInfoCharMacro(buf, byt);
}

/* --------------------------------
 *		pq_sendbytes	- append raw data to a StringInfo buffer
 * --------------------------------
 */
void
pq_sendbytes(StringInfo buf, const char *data, int datalen)
{
	appendBinaryStringInfo(buf, data, datalen);
}

/* --------------------------------
 *		pq_sendcountedtext - append a text string (with character set conversion)
 *
 * The data sent to the frontend by this routine is a 4-byte count field
 * (the count includes itself, by convention) followed by the string.
 * The passed text string need not be null-terminated, and the data sent
 * to the frontend isn't either.
 * --------------------------------
 */
void
pq_sendcountedtext(StringInfo buf, const char *str, int slen)
{
	char	   *p;

	p = (char *) pg_server_to_client((unsigned char *) str, slen);
	if (p != str)				/* actual conversion has been done? */
	{
		slen = strlen(p);
		pq_sendint(buf, slen + 4, 4);
		appendBinaryStringInfo(buf, p, slen);
		pfree(p);
		return;
	}
	pq_sendint(buf, slen + 4, 4);
	appendBinaryStringInfo(buf, str, slen);
}

/* --------------------------------
 *		pq_sendstring	- append a null-terminated text string (with conversion)
 *
 * NB: passed text string must be null-terminated, and so is the data
 * sent to the frontend.
 * --------------------------------
 */
void
pq_sendstring(StringInfo buf, const char *str)
{
	int			slen = strlen(str);

	char	   *p;

	p = (char *) pg_server_to_client((unsigned char *) str, slen);
	if (p != str)				/* actual conversion has been done? */
	{
		slen = strlen(p);
		appendBinaryStringInfo(buf, p, slen + 1);
		pfree(p);
		return;
	}
	appendBinaryStringInfo(buf, str, slen + 1);
}

/* --------------------------------
 *		pq_sendint		- append a binary integer to a StringInfo buffer
 * --------------------------------
 */
void
pq_sendint(StringInfo buf, int i, int b)
{
	unsigned char n8;
	uint16		n16;
	uint32		n32;

	switch (b)
	{
		case 1:
			n8 = (unsigned char) i;
			appendBinaryStringInfo(buf, (char *) &n8, 1);
			break;
		case 2:
			n16 = htons((uint16) i);
			appendBinaryStringInfo(buf, (char *) &n16, 2);
			break;
		case 4:
			n32 = htonl((uint32) i);
			appendBinaryStringInfo(buf, (char *) &n32, 4);
			break;
		default:
			elog(ERROR, "pq_sendint: unsupported size %d", b);
			break;
	}
}

/* --------------------------------
 *		pq_endmessage	- send the completed message to the frontend
 *
 * The data buffer is pfree()d, but if the StringInfo was allocated with
 * makeStringInfo then the caller must still pfree it.
 * --------------------------------
 */
void
pq_endmessage(StringInfo buf)
{
	(void) pq_putmessage('\0', buf->data, buf->len);
	/* no need to complain about any failure, since pqcomm.c already did */
	pfree(buf->data);
	buf->data = NULL;
}

/* --------------------------------
 *		pq_puttextmessage - generate a character set-converted message in one step
 *
 *		This is the same as the pqcomm.c routine pq_putmessage, except that
 *		the message body is a null-terminated string to which encoding
 *		conversion applies.
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
int
pq_puttextmessage(char msgtype, const char *str)
{
	int			slen = strlen(str);
	char	   *p;

	p = (char *) pg_server_to_client((unsigned char *) str, slen);
	if (p != str)				/* actual conversion has been done? */
	{
		int			result = pq_putmessage(msgtype, p, strlen(p) + 1);

		pfree(p);
		return result;
	}
	return pq_putmessage(msgtype, str, slen + 1);
}

/* --------------------------------
 *		pq_getint - get an integer from connection
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
int
pq_getint(int *result, int b)
{
	int			status;
	unsigned char n8;
	uint16		n16;
	uint32		n32;

	switch (b)
	{
		case 1:
			status = pq_getbytes((char *) &n8, 1);
			*result = (int) n8;
			break;
		case 2:
			status = pq_getbytes((char *) &n16, 2);
			*result = (int) (ntohs(n16));
			break;
		case 4:
			status = pq_getbytes((char *) &n32, 4);
			*result = (int) (ntohl(n32));
			break;
		default:

			/*
			 * if we elog(ERROR) here, we will lose sync with the
			 * frontend, so just complain to postmaster log instead...
			 */
			elog(COMMERROR, "pq_getint: unsupported size %d", b);
			status = EOF;
			*result = 0;
			break;
	}
	return status;
}

/* --------------------------------
 *		pq_getstr_bounded - get a null terminated string from connection
 *
 *		The return value is placed in an expansible StringInfo.
 *		Note that space allocation comes from the current memory context!
 *
 *		The maxlen parameter is interpreted as per pq_getstring.
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
int
pq_getstr_bounded(StringInfo s, int maxlen)
{
	int			result;
	char	   *p;

	result = pq_getstring(s, maxlen);

	p = (char *) pg_client_to_server((unsigned char *) s->data, s->len);
	if (p != s->data)			/* actual conversion has been done? */
	{
		/* reset s to empty, and append the new string p */
		s->len = 0;
		s->data[0] = '\0';
		appendBinaryStringInfo(s, p, strlen(p));
		pfree(p);
	}

	return result;
}
