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
 * character set conversion, while text is converted by character encoding
 * rules.
 *
 * Incoming messages are similarly read into a StringInfo buffer, via
 * pq_getmessage, and then parsed and converted from that using the routines
 * in this module.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$Header: /cvsroot/pgsql/src/backend/libpq/pqformat.c,v 1.29 2003/05/08 18:16:36 tgl Exp $
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
 *		pq_putemptymessage - convenience routine for message with empty body
 *
 * Message parsing after input:
 *		pq_getmsgbyte	- get a raw byte from a message buffer
 *		pq_getmsgint	- get a binary integer from a message buffer
 *		pq_getmsgbytes	- get raw data from a message buffer
 *		pq_copymsgbytes	- copy raw data from a message buffer
 *		pq_getmsgstring	- get a null-terminated text string (with conversion)
 *		pq_getmsgend	- verify message fully consumed
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
 *		pq_beginmessage		- initialize for sending a message
 * --------------------------------
 */
void
pq_beginmessage(StringInfo buf, char msgtype)
{
	initStringInfo(buf);
	/*
	 * We stash the message type into the buffer's cursor field, expecting
	 * that the pq_sendXXX routines won't touch it.  We could alternatively
	 * make it the first byte of the buffer contents, but this seems easier.
	 */
	buf->cursor = msgtype;
}

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
 * followed by the string.  The count includes itself or not, as per the
 * countincludesself flag (pre-3.0 protocol requires it to include itself).
 * The passed text string need not be null-terminated, and the data sent
 * to the frontend isn't either.
 * --------------------------------
 */
void
pq_sendcountedtext(StringInfo buf, const char *str, int slen,
				   bool countincludesself)
{
	int			extra = countincludesself ? 4 : 0;
	char	   *p;

	p = (char *) pg_server_to_client((unsigned char *) str, slen);
	if (p != str)				/* actual conversion has been done? */
	{
		slen = strlen(p);
		pq_sendint(buf, slen + extra, 4);
		appendBinaryStringInfo(buf, p, slen);
		pfree(p);
	}
	else
	{
		pq_sendint(buf, slen + extra, 4);
		appendBinaryStringInfo(buf, str, slen);
	}
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
	/* msgtype was saved in cursor field */
	(void) pq_putmessage(buf->cursor, buf->data, buf->len);
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
 * --------------------------------
 */
void
pq_puttextmessage(char msgtype, const char *str)
{
	int			slen = strlen(str);
	char	   *p;

	p = (char *) pg_server_to_client((unsigned char *) str, slen);
	if (p != str)				/* actual conversion has been done? */
	{
		(void) pq_putmessage(msgtype, p, strlen(p) + 1);
		pfree(p);
		return;
	}
	(void) pq_putmessage(msgtype, str, slen + 1);
}


/* --------------------------------
 *		pq_putemptymessage - convenience routine for message with empty body
 * --------------------------------
 */
void
pq_putemptymessage(char msgtype)
{
	(void) pq_putmessage(msgtype, NULL, 0);
}


/* --------------------------------
 *		pq_getmsgbyte	- get a raw byte from a message buffer
 * --------------------------------
 */
int
pq_getmsgbyte(StringInfo msg)
{
	if (msg->cursor >= msg->len)
		elog(ERROR, "pq_getmsgbyte: no data left in message");
	return (unsigned char) msg->data[msg->cursor++];
}

/* --------------------------------
 *		pq_getmsgint	- get a binary integer from a message buffer
 *
 *		Values are treated as unsigned.
 * --------------------------------
 */
unsigned int
pq_getmsgint(StringInfo msg, int b)
{
	unsigned int result;
	unsigned char n8;
	uint16		n16;
	uint32		n32;

	switch (b)
	{
		case 1:
			pq_copymsgbytes(msg, (char *) &n8, 1);
			result = n8;
			break;
		case 2:
			pq_copymsgbytes(msg, (char *) &n16, 2);
			result = ntohs(n16);
			break;
		case 4:
			pq_copymsgbytes(msg, (char *) &n32, 4);
			result = ntohl(n32);
			break;
		default:
			elog(ERROR, "pq_getmsgint: unsupported size %d", b);
			result = 0;			/* keep compiler quiet */
			break;
	}
	return result;
}

/* --------------------------------
 *		pq_getmsgbytes	- get raw data from a message buffer
 *
 *		Returns a pointer directly into the message buffer; note this
 *		may not have any particular alignment.
 * --------------------------------
 */
const char *
pq_getmsgbytes(StringInfo msg, int datalen)
{
	const char *result;

	if (datalen < 0 || datalen > (msg->len - msg->cursor))
		elog(ERROR, "pq_getmsgbytes: insufficient data left in message");
	result = &msg->data[msg->cursor];
	msg->cursor += datalen;
	return result;
}

/* --------------------------------
 *		pq_copymsgbytes	- copy raw data from a message buffer
 *
 *		Same as above, except data is copied to caller's buffer.
 * --------------------------------
 */
void
pq_copymsgbytes(StringInfo msg, char *buf, int datalen)
{
	if (datalen < 0 || datalen > (msg->len - msg->cursor))
		elog(ERROR, "pq_copymsgbytes: insufficient data left in message");
	memcpy(buf, &msg->data[msg->cursor], datalen);
	msg->cursor += datalen;
}

/* --------------------------------
 *		pq_getmsgstring	- get a null-terminated text string (with conversion)
 *
 *		May return a pointer directly into the message buffer, or a pointer
 *		to a palloc'd conversion result.
 * --------------------------------
 */
const char *
pq_getmsgstring(StringInfo msg)
{
	char   *str;
	int		slen;

	str = &msg->data[msg->cursor];
	/*
	 * It's safe to use strlen() here because a StringInfo is guaranteed
	 * to have a trailing null byte.  But check we found a null inside
	 * the message.
	 */
	slen = strlen(str);
	if (msg->cursor + slen >= msg->len)
		elog(ERROR, "pq_getmsgstring: invalid string in message");
	msg->cursor += slen + 1;

	return (const char *) pg_client_to_server((unsigned char *) str, slen);
}

/* --------------------------------
 *		pq_getmsgend	- verify message fully consumed
 * --------------------------------
 */
void
pq_getmsgend(StringInfo msg)
{
	if (msg->cursor != msg->len)
		elog(ERROR, "pq_getmsgend: invalid message format");
}
