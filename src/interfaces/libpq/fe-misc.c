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
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-misc.c,v 1.88 2003/04/02 00:49:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <errno.h>
#include <signal.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#include <sys/time.h>
#endif

#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "libpq-fe.h"
#include "libpq-int.h"
#include "pqsignal.h"
#include "mb/pg_wchar.h"

#define DONOTICE(conn,message) \
	((*(conn)->noticeHook) ((conn)->noticeArg, (message)))

static int	pqSocketCheck(PGconn *conn, int forRead, int forWrite,
						  time_t end_time);
static int	pqSocketPoll(int sock, int forRead, int forWrite, time_t end_time);
static int	pqPutBytes(const char *s, size_t nbytes, PGconn *conn);


/*
 * pqGetc:
 *	get a character from the connection
 *
 *	All these routines return 0 on success, EOF on error.
 *	Note that for the Get routines, EOF only means there is not enough
 *	data in the buffer, not that there is necessarily a hard error.
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


/*
 * write 1 char to the connection
 */
int
pqPutc(char c, PGconn *conn)
{
	if (pqPutBytes(&c, 1, conn) == EOF)
		return EOF;

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "To backend> %c\n", c);

	return 0;
}


/*
 * pqPutBytes: local routine to write N bytes to the connection,
 * with buffering
 */
static int
pqPutBytes(const char *s, size_t nbytes, PGconn *conn)
{
	/*
	 * Strategy to handle blocking and non-blocking connections: Fill the
	 * output buffer and flush it repeatedly until either all data has
	 * been sent or is at least queued in the buffer.
	 *
	 * For non-blocking connections, grow the buffer if not all data fits
	 * into it and the buffer can't be sent because the socket would
	 * block.
	 */

	while (nbytes)
	{
		size_t		avail,
					remaining;

		/* fill the output buffer */
		avail = Max(conn->outBufSize - conn->outCount, 0);
		remaining = Min(avail, nbytes);
		memcpy(conn->outBuffer + conn->outCount, s, remaining);
		conn->outCount += remaining;
		s += remaining;
		nbytes -= remaining;

		/*
		 * if the data didn't fit completely into the buffer, try to flush
		 * the buffer
		 */
		if (nbytes)
		{
			int			send_result = pqSendSome(conn);

			/* if there were errors, report them */
			if (send_result < 0)
				return EOF;

			/*
			 * if not all data could be sent, increase the output buffer,
			 * put the rest of s into it and return successfully. This
			 * case will only happen in a non-blocking connection
			 */
			if (send_result > 0)
			{
				/*
				 * try to grow the buffer. FIXME: The new size could be
				 * chosen more intelligently.
				 */
				size_t		buflen = (size_t) conn->outCount + nbytes;

				if (buflen > (size_t) conn->outBufSize)
				{
					char	   *newbuf = realloc(conn->outBuffer, buflen);

					if (!newbuf)
					{
						/* realloc failed. Probably out of memory */
						printfPQExpBuffer(&conn->errorMessage,
						   "cannot allocate memory for output buffer\n");
						return EOF;
					}
					conn->outBuffer = newbuf;
					conn->outBufSize = buflen;
				}
				/* put the data into it */
				memcpy(conn->outBuffer + conn->outCount, s, nbytes);
				conn->outCount += nbytes;

				/* report success. */
				return 0;
			}
		}

		/*
		 * pqSendSome was able to send all data. Continue with the next
		 * chunk of s.
		 */
	}							/* while */

	return 0;
}

/*
 * pqGets:
 * get a null-terminated string from the connection,
 * and store it in an expansible PQExpBuffer.
 * If we run out of memory, all of the string is still read,
 * but the excess characters are silently discarded.
 */
int
pqGets(PQExpBuffer buf, PGconn *conn)
{
	/* Copy conn data to locals for faster search loop */
	char	   *inBuffer = conn->inBuffer;
	int			inCursor = conn->inCursor;
	int			inEnd = conn->inEnd;
	int			slen;

	while (inCursor < inEnd && inBuffer[inCursor])
		inCursor++;

	if (inCursor >= inEnd)
		return EOF;

	slen = inCursor - conn->inCursor;

	resetPQExpBuffer(buf);
	appendBinaryPQExpBuffer(buf, inBuffer + conn->inCursor, slen);

	conn->inCursor = ++inCursor;

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "From backend> \"%s\"\n",
				buf->data);

	return 0;
}


int
pqPuts(const char *s, PGconn *conn)
{
	if (pqPutBytes(s, strlen(s) + 1, conn))
		return EOF;

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "To backend> %s\n", s);

	return 0;
}

/*
 * pqGetnchar:
 *	get a string of exactly len bytes in buffer s, no null termination
 */
int
pqGetnchar(char *s, size_t len, PGconn *conn)
{
	if (len < 0 || len > (size_t) (conn->inEnd - conn->inCursor))
		return EOF;

	memcpy(s, conn->inBuffer + conn->inCursor, len);
	/* no terminating null */

	conn->inCursor += len;

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "From backend (%lu)> %.*s\n", (unsigned long) len, (int) len, s);

	return 0;
}

/*
 * pqPutnchar:
 *	send a string of exactly len bytes, no null termination needed
 */
int
pqPutnchar(const char *s, size_t len, PGconn *conn)
{
	if (pqPutBytes(s, len, conn))
		return EOF;

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "To backend> %.*s\n", (int) len, s);

	return 0;
}

/*
 * pgGetInt
 *	read a 2 or 4 byte integer and convert from network byte order
 *	to local byte order
 */
int
pqGetInt(int *result, size_t bytes, PGconn *conn)
{
	uint16		tmp2;
	uint32		tmp4;
	char		noticeBuf[64];

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
			snprintf(noticeBuf, sizeof(noticeBuf),
					 libpq_gettext("integer of size %lu not supported by pqGetInt\n"),
					 (unsigned long) bytes);
			DONOTICE(conn, noticeBuf);
			return EOF;
	}

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "From backend (#%lu)> %d\n", (unsigned long) bytes, *result);

	return 0;
}

/*
 * pgPutInt
 * send an integer of 2 or 4 bytes, converting from host byte order
 * to network byte order.
 */
int
pqPutInt(int value, size_t bytes, PGconn *conn)
{
	uint16		tmp2;
	uint32		tmp4;
	char		noticeBuf[64];

	switch (bytes)
	{
		case 2:
			tmp2 = htons((uint16) value);
			if (pqPutBytes((const char *) &tmp2, 2, conn))
				return EOF;
			break;
		case 4:
			tmp4 = htonl((uint32) value);
			if (pqPutBytes((const char *) &tmp4, 4, conn))
				return EOF;
			break;
		default:
			snprintf(noticeBuf, sizeof(noticeBuf),
					 libpq_gettext("integer of size %lu not supported by pqPutInt\n"),
					 (unsigned long) bytes);
			DONOTICE(conn, noticeBuf);
			return EOF;
	}

	if (conn->Pfdebug)
		fprintf(conn->Pfdebug, "To backend (%lu#)> %d\n", (unsigned long) bytes, value);

	return 0;
}

/*
 * pqReadReady: is select() saying the file is ready to read?
 * JAB: -or- if SSL is enabled and used, is it buffering bytes?
 * Returns -1 on failure, 0 if not ready, 1 if ready.
 */
int
pqReadReady(PGconn *conn)
{
	return pqSocketCheck(conn, 1, 0, (time_t) 0);
}

/*
 * pqWriteReady: is select() saying the file is ready to write?
 * Returns -1 on failure, 0 if not ready, 1 if ready.
 */
int
pqWriteReady(PGconn *conn)
{
	return pqSocketCheck(conn, 0, 1, (time_t) 0);
}

/* ----------
 * pqReadData: read more data, if any is available
 * Possible return values:
 *	 1: successfully loaded at least one more byte
 *	 0: no data is presently available, but no error detected
 *	-1: error detected (including EOF = connection closure);
 *		conn->errorMessage set
 * NOTE: callers must not assume that pointers or indexes into conn->inBuffer
 * remain valid across this call!
 * ----------
 */
int
pqReadData(PGconn *conn)
{
	int			someread = 0;
	int			nread;

	if (conn->sock < 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("connection not open\n"));
		return -1;
	}

	/* Left-justify any data in the buffer to make room */
	if (conn->inStart < conn->inEnd)
	{
		if (conn->inStart > 0)
		{
			memmove(conn->inBuffer, conn->inBuffer + conn->inStart,
					conn->inEnd - conn->inStart);
			conn->inEnd -= conn->inStart;
			conn->inCursor -= conn->inStart;
			conn->inStart = 0;
		}
	}
	else
	{
		/* buffer is logically empty, reset it */
		conn->inStart = conn->inCursor = conn->inEnd = 0;
	}

	/*
	 * If the buffer is fairly full, enlarge it. We need to be able to
	 * enlarge the buffer in case a single message exceeds the initial
	 * buffer size.  We enlarge before filling the buffer entirely so as
	 * to avoid asking the kernel for a partial packet. The magic constant
	 * here should be large enough for a TCP packet or Unix pipe
	 * bufferload.	8K is the usual pipe buffer size, so...
	 */
	if (conn->inBufSize - conn->inEnd < 8192)
	{
		int			newSize = conn->inBufSize * 2;
		char	   *newBuf = (char *) realloc(conn->inBuffer, newSize);

		if (newBuf)
		{
			conn->inBuffer = newBuf;
			conn->inBufSize = newSize;
		}
	}

	/* OK, try to read some data */
retry3:
	nread = pqsecure_read(conn, conn->inBuffer + conn->inEnd,
						  conn->inBufSize - conn->inEnd);
	if (nread < 0)
	{
		if (SOCK_ERRNO == EINTR)
			goto retry3;
		/* Some systems return EAGAIN/EWOULDBLOCK for no data */
#ifdef EAGAIN
		if (SOCK_ERRNO == EAGAIN)
			return someread;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
		if (SOCK_ERRNO == EWOULDBLOCK)
			return someread;
#endif
		/* We might get ECONNRESET here if using TCP and backend died */
#ifdef ECONNRESET
		if (SOCK_ERRNO == ECONNRESET)
			goto definitelyFailed;
#endif
		printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not receive data from server: %s\n"),
						  SOCK_STRERROR(SOCK_ERRNO));
		return -1;
	}
	if (nread > 0)
	{
		conn->inEnd += nread;

		/*
		 * Hack to deal with the fact that some kernels will only give us
		 * back 1 packet per recv() call, even if we asked for more and
		 * there is more available.  If it looks like we are reading a
		 * long message, loop back to recv() again immediately, until we
		 * run out of data or buffer space.  Without this, the
		 * block-and-restart behavior of libpq's higher levels leads to
		 * O(N^2) performance on long messages.
		 *
		 * Since we left-justified the data above, conn->inEnd gives the
		 * amount of data already read in the current message.	We
		 * consider the message "long" once we have acquired 32k ...
		 */
		if (conn->inEnd > 32768 &&
			(conn->inBufSize - conn->inEnd) >= 8192)
		{
			someread = 1;
			goto retry3;
		}
		return 1;
	}

	if (someread)
		return 1;				/* got a zero read after successful tries */

	/*
	 * A return value of 0 could mean just that no data is now available,
	 * or it could mean EOF --- that is, the server has closed the
	 * connection. Since we have the socket in nonblock mode, the only way
	 * to tell the difference is to see if select() is saying that the
	 * file is ready. Grumble.	Fortunately, we don't expect this path to
	 * be taken much, since in normal practice we should not be trying to
	 * read data unless the file selected for reading already.
	 */
	switch (pqReadReady(conn))
	{
		case 0:
			/* definitely no data available */
			return 0;
		case 1:
			/* ready for read */
			break;
		default:
			goto definitelyFailed;
	}

	/*
	 * Still not sure that it's EOF, because some data could have just
	 * arrived.
	 */
retry4:
	nread = pqsecure_read(conn, conn->inBuffer + conn->inEnd,
						  conn->inBufSize - conn->inEnd);
	if (nread < 0)
	{
		if (SOCK_ERRNO == EINTR)
			goto retry4;
		/* Some systems return EAGAIN/EWOULDBLOCK for no data */
#ifdef EAGAIN
		if (SOCK_ERRNO == EAGAIN)
			return 0;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
		if (SOCK_ERRNO == EWOULDBLOCK)
			return 0;
#endif
		/* We might get ECONNRESET here if using TCP and backend died */
#ifdef ECONNRESET
		if (SOCK_ERRNO == ECONNRESET)
			goto definitelyFailed;
#endif
		printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not receive data from server: %s\n"),
						  SOCK_STRERROR(SOCK_ERRNO));
		return -1;
	}
	if (nread > 0)
	{
		conn->inEnd += nread;
		return 1;
	}

	/*
	 * OK, we are getting a zero read even though select() says ready.
	 * This means the connection has been closed.  Cope.
	 */
definitelyFailed:
	printfPQExpBuffer(&conn->errorMessage,
					  libpq_gettext(
							"server closed the connection unexpectedly\n"
			   "\tThis probably means the server terminated abnormally\n"
						 "\tbefore or while processing the request.\n"));
	conn->status = CONNECTION_BAD;		/* No more connection to backend */
	pqsecure_close(conn);
#ifdef WIN32
	closesocket(conn->sock);
#else
	close(conn->sock);
#endif
	conn->sock = -1;

	return -1;
}

/*
 * pqSendSome: send any data waiting in the output buffer.
 *
 * Return 0 on sucess, -1 on failure and 1 when data remains because the
 * socket would block and the connection is non-blocking.
 */
int
pqSendSome(PGconn *conn)
{
	char	   *ptr = conn->outBuffer;
	int			len = conn->outCount;

	if (conn->sock < 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("connection not open\n"));
		return -1;
	}

	/*
	 * don't try to send zero data, allows us to use this function without
	 * too much worry about overhead
	 */
	if (len == 0)
		return (0);

	/* while there's still data to send */
	while (len > 0)
	{
		int			sent;

		sent = pqsecure_write(conn, ptr, len);

		if (sent < 0)
		{
			/*
			 * Anything except EAGAIN or EWOULDBLOCK is trouble. If it's
			 * EPIPE or ECONNRESET, assume we've lost the backend
			 * connection permanently.
			 */
			switch (SOCK_ERRNO)
			{
#ifdef EAGAIN
				case EAGAIN:
					break;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
				case EWOULDBLOCK:
					break;
#endif
				case EINTR:
					continue;

				case EPIPE:
#ifdef ECONNRESET
				case ECONNRESET:
#endif
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext(
							"server closed the connection unexpectedly\n"
													"\tThis probably means the server terminated abnormally\n"
						 "\tbefore or while processing the request.\n"));

					/*
					 * We used to close the socket here, but that's a bad
					 * idea since there might be unread data waiting
					 * (typically, a NOTICE message from the backend
					 * telling us it's committing hara-kiri...).  Leave
					 * the socket open until pqReadData finds no more data
					 * can be read.
					 */
					return -1;

				default:
					printfPQExpBuffer(&conn->errorMessage,
					libpq_gettext("could not send data to server: %s\n"),
									  SOCK_STRERROR(SOCK_ERRNO));
					/* We don't assume it's a fatal error... */
					return -1;
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

			/*
			 * if the socket is in non-blocking mode we may need to abort
			 * here and return 1 to indicate that data is still pending.
			 */
#ifdef USE_SSL
			/* can't do anything for our SSL users yet */
			if (conn->ssl == NULL)
			{
#endif
				if (pqIsnonblocking(conn))
				{
					/* shift the contents of the buffer */
					memmove(conn->outBuffer, ptr, len);
					conn->outCount = len;
					return 1;
				}
#ifdef USE_SSL
			}
#endif

			if (pqWait(FALSE, TRUE, conn))
				return -1;
		}
	}

	conn->outCount = 0;

	if (conn->Pfdebug)
		fflush(conn->Pfdebug);

	return 0;
}



/*
 * pqFlush: send any data waiting in the output buffer
 *
 * Implemented in terms of pqSendSome to recreate the old behavior which
 * returned 0 if all data was sent or EOF. EOF was sent regardless of
 * whether an error occurred or not all data was sent on a non-blocking
 * socket.
 */
int
pqFlush(PGconn *conn)
{
	if (pqSendSome(conn))
		return EOF;
	return 0;
}

/*
 * pqWait: wait until we can read or write the connection socket
 *
 * JAB: If SSL enabled and used and forRead, buffered bytes short-circuit the
 * call to select().
 *
 * We also stop waiting and return if the kernel flags an exception condition
 * on the socket.  The actual error condition will be detected and reported
 * when the caller tries to read or write the socket.
 */
int
pqWait(int forRead, int forWrite, PGconn *conn)
{
	return pqWaitTimed(forRead, forWrite, conn, (time_t) -1);
}

/*
 * pqWaitTimed: wait, but not past finish_time.
 *
 * If finish_time is exceeded then we return failure (EOF).  This is like
 * the response for a kernel exception because we don't want the caller
 * to try to read/write in that case.
 *
 * finish_time = ((time_t) -1) disables the wait limit.
 */
int
pqWaitTimed(int forRead, int forWrite, PGconn *conn, time_t finish_time)
{
	int result;

	result = pqSocketCheck(conn, forRead, forWrite, finish_time);

	if (result < 0)
		return EOF;				/* errorMessage is already set */

	if (result == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("timeout expired\n"));
		return EOF;
	}

	return 0;
}

/*
 * Checks a socket, using poll or select, for data to be read, written,
 * or both.  Returns >0 if one or more conditions are met, 0 if it timed
 * out, -1 if an error occurred.
 * If SSL is in use, the SSL buffer is checked prior to checking the socket
 * for read data directly.
 */
static int
pqSocketCheck(PGconn *conn, int forRead, int forWrite, time_t end_time)
{
	int result;

	if (!conn)
		return -1;
	if (conn->sock < 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
		                  libpq_gettext("socket not open\n"));
		return -1;
	}

/* JAB: Check for SSL library buffering read bytes */
#ifdef USE_SSL
	if (forRead && conn->ssl && SSL_pending(conn->ssl) > 0)
	{
		/* short-circuit the select */
		return 1;
	}
#endif

	/* We will retry as long as we get EINTR */
	do
	{
		result = pqSocketPoll(conn->sock, forRead, forWrite, end_time);
	}
	while (result < 0 && SOCK_ERRNO == EINTR);

	if (result < 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
		                  libpq_gettext("select() failed: %s\n"),
		                  SOCK_STRERROR(SOCK_ERRNO));
	}

	return result;
}


/*
 * Check a file descriptor for read and/or write data, possibly waiting.
 * If neither forRead nor forWrite are set, immediately return a timeout
 * condition (without waiting).  Return >0 if condition is met, 0
 * if a timeout occurred, -1 if an error or interrupt occurred.
 * Timeout is infinite if end_time is -1.  Timeout is immediate (no blocking)
 * if end_time is 0 (or indeed, any time before now).
 */
static int
pqSocketPoll(int sock, int forRead, int forWrite, time_t end_time)
{
	/* We use poll(2) if available, otherwise select(2) */
#ifdef HAVE_POLL
	struct pollfd input_fd;
	int           timeout_ms;

	input_fd.fd      = sock;
	input_fd.events  = 0;
	input_fd.revents = 0;

	if (forRead)
		input_fd.events |= POLLIN;
	if (forWrite)
		input_fd.events |= POLLOUT;
	if (!input_fd.events)
		return 0;

	/* Compute appropriate timeout interval */
	if (end_time == ((time_t) -1))
	{
		timeout_ms = -1;
	}
	else
	{
		time_t now = time(NULL);

		if (end_time > now)
			timeout_ms = (end_time - now) * 1000;
		else
			timeout_ms = 0;
	}

	return poll(&input_fd, 1, timeout_ms);

#else /* !HAVE_POLL */

	fd_set          input_mask;
	fd_set          output_mask;
	fd_set          except_mask;
	struct timeval  timeout;
	struct timeval *ptr_timeout;

	if (!forRead && !forWrite)
		return 0;

	FD_ZERO(&input_mask);
	FD_ZERO(&output_mask);
	FD_ZERO(&except_mask);
	if (forRead)
		FD_SET(sock, &input_mask);
	if (forWrite)
		FD_SET(sock, &output_mask);
	FD_SET(sock, &except_mask);

	/* Compute appropriate timeout interval */
	if (end_time == ((time_t) -1))
	{
		ptr_timeout = NULL;
	}
	else
	{
		time_t	now = time(NULL);

		if (end_time > now)
			timeout.tv_sec = end_time - now;
		else
			timeout.tv_sec = 0;
		timeout.tv_usec = 0;
		ptr_timeout = &timeout;
	}

	return select(sock + 1, &input_mask, &output_mask,
				  &except_mask, ptr_timeout);
#endif /* HAVE_POLL */
}


/*
 * A couple of "miscellaneous" multibyte related functions. They used
 * to be in fe-print.c but that file is doomed.
 */

/*
 * returns the byte length of the word beginning s, using the
 * specified encoding.
 */
int
PQmblen(const unsigned char *s, int encoding)
{
	return (pg_encoding_mblen(encoding, s));
}

/*
 * Get encoding id from environment variable PGCLIENTENCODING.
 */
int
PQenv2encoding(void)
{
	char	   *str;
	int			encoding = PG_SQL_ASCII;

	str = getenv("PGCLIENTENCODING");
	if (str && *str != '\0')
		encoding = pg_char_to_encoding(str);
	return (encoding);
}


#ifdef ENABLE_NLS

char *
libpq_gettext(const char *msgid)
{
	static int	already_bound = 0;

	if (!already_bound)
	{
		already_bound = 1;
		bindtextdomain("libpq", LOCALEDIR);
	}

	return dgettext("libpq", msgid);
}

#endif   /* ENABLE_NLS */
