/*-------------------------------------------------------------------------
 *
 *	 FILE
 *		fe-misc.c
 *
 *	 DESCRIPTION
 *		 miscellaneous useful functions
 *
 * The communication routines here are analogous to the ones in
 * backend/libpq/pqcomm.c and backend/libpq/pqcomprim.c, but operate
 * in the considerably different environment of the frontend libpq.
 * In particular, we work with a bare nonblock-mode socket, rather than
 * a stdio stream, so that we can avoid unwanted blocking of the application.
 *
 * XXX: MOVE DEBUG PRINTOUT TO HIGHER LEVEL.  As is, block and restart
 * will cause repeat printouts.
 *
 * We must speak the same transmitted data representations as the backend
 * routines.  Note that this module supports *only* network byte order
 * for transmitted ints, whereas the backend modules (as of this writing)
 * still handle either network or little-endian byte order.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-misc.c,v 1.15 1998/06/15 19:30:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#if !defined(NO_UNISTD_H)
#include <unistd.h>
#endif
#include <sys/types.h>			/* for fd_set stuff */
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "postgres.h"
#include "libpq-fe.h"

/* --------------------------------------------------------------------- */
/* pqGetc:
   get a character from the connection

   All these routines return 0 on success, EOF on error.
   Note that for the Get routines, EOF only means there is not enough
   data in the buffer, not that there is necessarily a hard error.
*/
int
pqGetc(char *result, PGconn *conn)
{
	if (conn->inCursor >= conn->inEnd)
		return EOF;

	*result = conn->inBuffer[conn->inCursor++];

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "From backend> %c\n", *result);

	return 0;
}


/* --------------------------------------------------------------------- */
/* pqPutBytes: local routine to write N bytes to the connection,
   with buffering
 */
static int
pqPutBytes(const char *s, int nbytes, PGconn *conn)
{
	int avail = conn->outBufSize - conn->outCount;

	while (nbytes > avail)
	{
		memcpy(conn->outBuffer + conn->outCount, s, avail);
		conn->outCount += avail;
		s += avail;
		nbytes -= avail;
		if (pqFlush(conn))
			return EOF;
		avail = conn->outBufSize;
	}

	memcpy(conn->outBuffer + conn->outCount, s, nbytes);
	conn->outCount += nbytes;

	return 0;
}

/* --------------------------------------------------------------------- */
/* pqGets:
   get a null-terminated string from the connection,
   and store it in a buffer of size maxlen bytes.
   If the incoming string is >= maxlen bytes, all of it is read,
   but the excess characters are silently discarded.
*/
int
pqGets(char *s, int maxlen, PGconn *conn)
{
	/* Copy conn data to locals for faster search loop */
	char	*inBuffer = conn->inBuffer;
	int		inCursor = conn->inCursor;
	int		inEnd = conn->inEnd;
	int		slen;

	while (inCursor < inEnd && inBuffer[inCursor])
		inCursor++;

	if (inCursor >= inEnd)
		return EOF;

	slen = inCursor - conn->inCursor;
	if (slen < maxlen)
		strcpy(s, inBuffer + conn->inCursor);
	else
	{
		strncpy(s, inBuffer + conn->inCursor, maxlen-1);
		s[maxlen-1] = '\0';
	}

	conn->inCursor = ++inCursor;

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "From backend> \"%s\"\n", s);

	return 0;
}

/* --------------------------------------------------------------------- */
int
pqPuts(const char *s, PGconn *conn)
{
	if (pqPutBytes(s, strlen(s)+1, conn))
		return EOF;

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "To backend> %s\n", s);

	return 0;
}

/* --------------------------------------------------------------------- */
/* pqGetnchar:
   get a string of exactly len bytes in buffer s (which must be 1 byte
   longer) and terminate it with a '\0'.
*/
int
pqGetnchar(char *s, int len, PGconn *conn)
{
	if (len < 0 || len > conn->inEnd - conn->inCursor)
		return EOF;

	memcpy(s, conn->inBuffer + conn->inCursor, len);
	s[len] = '\0';

	conn->inCursor += len;

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "From backend (%d)> %s\n", len, s);

	return 0;
}

/* --------------------------------------------------------------------- */
/* pqPutnchar:
   send a string of exactly len bytes
   The buffer should have a terminating null, but it's not sent.
*/
int
pqPutnchar(const char *s, int len, PGconn *conn)
{
	if (pqPutBytes(s, len, conn))
		return EOF;

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "To backend> %s\n", s);

	return 0;
}

/* --------------------------------------------------------------------- */
/* pgGetInt
   read a 2 or 4 byte integer and convert from network byte order
   to local byte order
*/
int
pqGetInt(int *result, int bytes, PGconn *conn)
{
	uint16 tmp2;
	uint32 tmp4;

	switch (bytes)
	{
		case 2:
			if (conn->inCursor + 2 > conn->inEnd)
				return EOF;
			memcpy(&tmp2, conn->inBuffer + conn->inCursor, 2);
			conn->inCursor += 2;
			*result = (int) ntohs(tmp2);
			break;
		case 4:
			if (conn->inCursor + 4 > conn->inEnd)
				return EOF;
			memcpy(&tmp4, conn->inBuffer + conn->inCursor, 4);
			conn->inCursor += 4;
			*result = (int) ntohl(tmp4);
			break;
		default:
			fprintf(stderr, "** int size %d not supported\n", bytes);
			return EOF;
	}

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "From backend (#%d)> %d\n", bytes, *result);

	return 0;
}

/* --------------------------------------------------------------------- */
/* pgPutInt
   send an integer of 2 or 4 bytes, converting from host byte order
   to network byte order.
*/
int
pqPutInt(int value, int bytes, PGconn *conn)
{
	uint16 tmp2;
	uint32 tmp4;

	switch (bytes)
	{
		case 2:
			tmp2 = htons((uint16) value);
			if (pqPutBytes((const char*) &tmp2, 2, conn))
				return EOF;
			break;
		case 4:
			tmp4 = htonl((uint32) value);
			if (pqPutBytes((const char*) &tmp4, 4, conn))
				return EOF;
			break;
		default:
			fprintf(stderr, "** int size %d not supported\n", bytes);
			return EOF;
	}

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "To backend (%d#)> %d\n", bytes, value);

	return 0;
}

/* --------------------------------------------------------------------- */
/* pqReadReady: is select() saying the file is ready to read?
 */
static int
pqReadReady(PGconn *conn)
{
	fd_set			input_mask;
	struct timeval	timeout;

	if (conn->sock < 0)
		return 0;

	FD_ZERO(&input_mask);
	FD_SET(conn->sock, &input_mask);
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	if (select(conn->sock+1, &input_mask, (fd_set *) NULL, (fd_set *) NULL,
			   &timeout) < 0)
	{
		sprintf(conn->errorMessage,
				"pqReadReady() -- select() failed: errno=%d\n%s\n",
				errno, strerror(errno));
		return 0;
	}
	return FD_ISSET(conn->sock, &input_mask);
}

/* --------------------------------------------------------------------- */
/* pqReadData: read more data, if any is available
 * Possible return values:
 *   1: successfully loaded at least one more byte
 *   0: no data is presently available, but no error detected
 *  -1: error detected (including EOF = connection closure);
 *      conn->errorMessage set
 * NOTE: callers must not assume that pointers or indexes into conn->inBuffer
 * remain valid across this call!
 */
int
pqReadData(PGconn *conn)
{
	int nread;

	if (conn->sock < 0)
	{
		strcpy(conn->errorMessage, "pqReadData() -- connection not open\n");
		return -1;
	}

	/* Left-justify any data in the buffer to make room */
	if (conn->inStart < conn->inEnd)
	{
		memmove(conn->inBuffer, conn->inBuffer + conn->inStart,
				conn->inEnd - conn->inStart);
		conn->inEnd -= conn->inStart;
		conn->inCursor -= conn->inStart;
		conn->inStart = 0;
	}
	else
		conn->inStart = conn->inCursor = conn->inEnd = 0;
	/* If the buffer is fairly full, enlarge it.
	 * We need to be able to enlarge the buffer in case a single message
	 * exceeds the initial buffer size.  We enlarge before filling the
	 * buffer entirely so as to avoid asking the kernel for a partial packet.
	 * The magic constant here should be at least one TCP packet.
	 */
	if (conn->inBufSize - conn->inEnd < 2000)
	{
		int newSize = conn->inBufSize * 2;
		char * newBuf = (char *) realloc(conn->inBuffer, newSize);
		if (newBuf)
		{
			conn->inBuffer = newBuf;
			conn->inBufSize = newSize;
		}
	}

	/* OK, try to read some data */
tryAgain:
	nread = recv(conn->sock, conn->inBuffer + conn->inEnd, 
				 conn->inBufSize - conn->inEnd, 0);
	if (nread < 0)
	{
		if (errno == EINTR)
			goto tryAgain;
		/* Some systems return EAGAIN/EWOULDBLOCK for no data */
#ifdef EAGAIN
		if (errno == EAGAIN)
			return 0;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
		if (errno == EWOULDBLOCK)
			return 0;
#endif
		sprintf(conn->errorMessage,
				"pqReadData() --  read() failed: errno=%d\n%s\n",
				errno, strerror(errno));
		return -1;
	}
	if (nread > 0)
	{
		conn->inEnd += nread;
		return 1;
	}

	/* A return value of 0 could mean just that no data is now available,
	 * or it could mean EOF --- that is, the server has closed the connection.
	 * Since we have the socket in nonblock mode, the only way to tell the
	 * difference is to see if select() is saying that the file is ready.
	 * Grumble.  Fortunately, we don't expect this path to be taken much,
	 * since in normal practice we should not be trying to read data unless
	 * the file selected for reading already.
	 */
	if (! pqReadReady(conn))
		return 0;				/* definitely no data available */

	/* Still not sure that it's EOF,
	 * because some data could have just arrived.
	 */
tryAgain2:
	nread = recv(conn->sock, conn->inBuffer + conn->inEnd, 
				 conn->inBufSize - conn->inEnd, 0);
	if (nread < 0)
	{
		if (errno == EINTR)
			goto tryAgain2;
		/* Some systems return EAGAIN/EWOULDBLOCK for no data */
#ifdef EAGAIN
		if (errno == EAGAIN)
			return 0;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
		if (errno == EWOULDBLOCK)
			return 0;
#endif
		sprintf(conn->errorMessage,
				"pqReadData() --  read() failed: errno=%d\n%s\n",
				errno, strerror(errno));
		return -1;
	}
	if (nread > 0)
	{
		conn->inEnd += nread;
		return 1;
	}

	/* OK, we are getting a zero read even though select() says ready.
	 * This means the connection has been closed.  Cope.
	 */
	sprintf(conn->errorMessage,
			"pqReadData() -- backend closed the channel unexpectedly.\n"
			"\tThis probably means the backend terminated abnormally"
			" before or while processing the request.\n");
	conn->status = CONNECTION_BAD; /* No more connection to
									* backend */
	close(conn->sock);
	conn->sock = -1;
	
	return -1;
}

/* --------------------------------------------------------------------- */
/* pqFlush: send any data waiting in the output buffer
 */
int
pqFlush(PGconn *conn)
{
	char * ptr = conn->outBuffer;
	int len = conn->outCount;

	if (conn->sock < 0)
	{
		strcpy(conn->errorMessage, "pqFlush() -- connection not open\n");
		return EOF;
	}

	while (len > 0)
	{
		int sent = send(conn->sock, ptr, len, 0);
		if (sent < 0)
		{
			/* Anything except EAGAIN or EWOULDBLOCK is trouble */
			switch (errno)
			{
#ifdef EAGAIN
				case EAGAIN:
					break;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
				case EWOULDBLOCK:
					break;
#endif
				default:
					sprintf(conn->errorMessage,
							"pqFlush() --  couldn't send data: errno=%d\n%s\n",
							errno, strerror(errno));
					return EOF;
			}
		}
		else
		{
			ptr += sent;
			len -= sent;
		}
		if (len > 0)
		{
			/* We didn't send it all, wait till we can send more */
			if (pqWait(FALSE, TRUE, conn))
				return EOF;
		}
	}

	conn->outCount = 0;

	if (conn->Pfdebug)
		fflush(conn->Pfdebug);

	return 0;
}

/* --------------------------------------------------------------------- */
/* pqWait: wait until we can read or write the connection socket
 */
int
pqWait(int forRead, int forWrite, PGconn *conn)
{
	fd_set	input_mask;
	fd_set	output_mask;

	if (conn->sock < 0)
	{
		strcpy(conn->errorMessage, "pqWait() -- connection not open\n");
		return EOF;
	}

	/* loop in case select returns EINTR */
	for (;;) {
		FD_ZERO(&input_mask);
		FD_ZERO(&output_mask);
		if (forRead)
			FD_SET(conn->sock, &input_mask);
		if (forWrite)
			FD_SET(conn->sock, &output_mask);
		if (select(conn->sock+1, &input_mask, &output_mask, (fd_set *) NULL,
				   (struct timeval *) NULL) < 0)
		{
			if (errno == EINTR)
				continue;
			sprintf(conn->errorMessage,
					"pqWait() -- select() failed: errno=%d\n%s\n",
					errno, strerror(errno));
			return EOF;
		}
		/* On nonerror return, assume we're done */
		break;
	}

	return 0;
}
