/*-------------------------------------------------------------------------
 *
 * pqcomm.c
 *	  Communication functions between the Frontend and the Backend
 *
 * These routines handle the low-level details of communication between
 * frontend and backend.  They just shove data across the communication
 * channel, and are ignorant of the semantics of the data --- or would be,
 * except for major brain damage in the design of the COPY OUT protocol.
 * Unfortunately, COPY OUT is designed to commandeer the communication
 * channel (it just transfers data without wrapping it into messages).
 * No other messages can be sent while COPY OUT is in progress; and if the
 * copy is aborted by an elog(ERROR), we need to close out the copy so that
 * the frontend gets back into sync.  Therefore, these routines have to be
 * aware of COPY OUT state.
 *
 * NOTE: generally, it's a bad idea to emit outgoing messages directly with
 * pq_putbytes(), especially if the message would require multiple calls
 * to send.  Instead, use the routines in pqformat.c to construct the message
 * in a buffer and then emit it in one call to pq_putmessage.  This helps
 * ensure that the channel will not be clogged by an incomplete message
 * if execution is aborted by elog(ERROR) partway through the message.
 * The only non-libpq code that should call pq_putbytes directly is COPY OUT.
 *
 * At one time, libpq was shared between frontend and backend, but now
 * the backend's "backend/libpq" is quite separate from "interfaces/libpq".
 * All that remains is similarities of names to trap the unwary...
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *  $Id: pqcomm.c,v 1.68 1999/04/25 03:19:21 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

/*------------------------
 * INTERFACE ROUTINES
 *
 * setup/teardown:
 *		StreamServerPort	- Open postmaster's server port
 *		StreamConnection	- Create new connection with client
 *		StreamClose			- Close a client/backend connection
 *		pq_getport		- return the PGPORT setting
 *		pq_init			- initialize libpq at backend startup
 *		pq_close		- shutdown libpq at backend exit
 *
 * low-level I/O:
 *		pq_getbytes		- get a known number of bytes from connection
 *		pq_getstring	- get a null terminated string from connection
 *		pq_peekbyte		- peek at next byte from connection
 *		pq_putbytes		- send bytes to connection (not flushed until pq_flush)
 *		pq_flush		- flush pending output
 *
 * message-level I/O (and COPY OUT cruft):
 *		pq_putmessage	- send a normal message (suppressed in COPY OUT mode)
 *		pq_startcopyout	- inform libpq that a COPY OUT transfer is beginning
 *		pq_endcopyout	- end a COPY OUT transfer
 *
 *------------------------
 */
#include "postgres.h"

#include <stdio.h>
#if defined(HAVE_STRING_H)
#include <string.h>
#else
#include <strings.h>
#endif
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>				/* for ttyname() */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/file.h>

#include "libpq/libpq.h"		/* where my declarations go */
#include "miscadmin.h"
#include "libpq/pqsignal.h"
#include "libpq/auth.h"
#include "storage/ipc.h"
#include "utils/trace.h"

#ifndef SOMAXCONN
#define SOMAXCONN 5				/* from Linux listen(2) man page */
#endif	 /* SOMAXCONN */

extern FILE * debug_port; /* in util.c */

/*
 * Buffers for low-level I/O
 */

#define PQ_BUFFER_SIZE 8192

static unsigned char PqSendBuffer[PQ_BUFFER_SIZE];
static int PqSendPointer;	/* Next index to store a byte in PqSendBuffer */

static unsigned char PqRecvBuffer[PQ_BUFFER_SIZE];
static int PqRecvPointer;	/* Next index to read a byte from PqRecvBuffer */
static int PqRecvLength;	/* End of data available in PqRecvBuffer */

/*
 * Message status
 */
static bool DoingCopyOut;


/* --------------------------------
 *		pq_init - initialize libpq at backend startup
 * --------------------------------
 */
void
pq_init(void)
{
	PqSendPointer = PqRecvPointer = PqRecvLength = 0;
	DoingCopyOut = false;
	if (getenv("LIBPQ_DEBUG"))
	  debug_port = stderr;
}

/* --------------------------------
 *		pq_getport - return the PGPORT setting
 * --------------------------------
 */
int
pq_getport(void)
{
	char	   *envport = getenv("PGPORT");

	if (envport)
		return atoi(envport);
	return atoi(DEF_PGPORT);
}

/* --------------------------------
 *		pq_close - shutdown libpq at backend exit
 * --------------------------------
 */
void
pq_close(void)
{
	close(MyProcPort->sock);
}



/*
 * Streams -- wrapper around Unix socket system calls
 *
 *
 *		Stream functions are used for vanilla TCP connection protocol.
 */

static char sock_path[MAXPGPATH + 1] = "";

/* StreamDoUnlink()
 * Shutdown routine for backend connection
 * If a Unix socket is used for communication, explicitly close it.
 */
static void
StreamDoUnlink()
{
	Assert(sock_path[0]);
	unlink(sock_path);
}

/*
 * StreamServerPort -- open a sock stream "listening" port.
 *
 * This initializes the Postmaster's connection-accepting port.
 *
 * RETURNS: STATUS_OK or STATUS_ERROR
 */

int
StreamServerPort(char *hostName, short portName, int *fdP)
{
	SockAddr	saddr;
	int			fd,
				err,
				family;
	size_t		len;
	int			one = 1;
#ifdef HAVE_FCNTL_SETLK
	int			lock_fd;
#endif

	family = ((hostName != NULL) ? AF_INET : AF_UNIX);

	if ((fd = socket(family, SOCK_STREAM, 0)) < 0)
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"FATAL: StreamServerPort: socket() failed: errno=%d\n",
				errno);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return STATUS_ERROR;
	}
	if ((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one,
					sizeof(one))) == -1)
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH, 
				"FATAL: StreamServerPort: setsockopt (SO_REUSEADDR) failed: errno=%d\n",
				errno);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return STATUS_ERROR;
	}
	MemSet((char *) &saddr, 0, sizeof(saddr));
	saddr.sa.sa_family = family;
	if (family == AF_UNIX)
	{
		len = UNIXSOCK_PATH(saddr.un, portName);
		strcpy(sock_path, saddr.un.sun_path);

		/*
		 * If the socket exists but nobody has an advisory lock on it we
		 * can safely delete the file.
		 */
#ifdef HAVE_FCNTL_SETLK
#ifndef __CYGWIN32__
		if ((lock_fd = open(sock_path, O_WRONLY | O_NONBLOCK, 0666)) >= 0)
#else
		if ((lock_fd = open(sock_path, O_WRONLY | O_NONBLOCK | O_BINARY, 0666)) >= 0)
#endif
		{
			struct flock	lck;
			
			lck.l_whence = SEEK_SET; lck.l_start = lck.l_len = 0;
			lck.l_type = F_WRLCK;
			if (fcntl(lock_fd, F_SETLK, &lck) == 0)
			{
				TPRINTF(TRACE_VERBOSE, "flock on %s, deleting", sock_path);
				unlink(sock_path);
			}
			else
				TPRINTF(TRACE_VERBOSE, "flock failed for %s", sock_path);
			close(lock_fd);
		}
#endif /* HAVE_FCNTL_SETLK */
	}
	else
	{
		saddr.in.sin_addr.s_addr = htonl(INADDR_ANY);
		saddr.in.sin_port = htons(portName);
		len = sizeof(struct sockaddr_in);
	}
	err = bind(fd, &saddr.sa, len);
	if (err < 0)
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"FATAL: StreamServerPort: bind() failed: errno=%d\n", errno);
		pqdebug("%s", PQerrormsg);
		strcat(PQerrormsg,
			   "\tIs another postmaster already running on that port?\n");
		if (family == AF_UNIX)
		{
			snprintf(PQerrormsg + strlen(PQerrormsg), ERROR_MSG_LENGTH,
					"\tIf not, remove socket node (%s) and retry.\n", sock_path);
		}
		else
		{
			strcat(PQerrormsg, "\tIf not, wait a few seconds and retry.\n");
		}
		fputs(PQerrormsg, stderr);
		return STATUS_ERROR;
	}

	if (family == AF_UNIX)
	{
		on_proc_exit(StreamDoUnlink, NULL);

		/*
		 * Open the socket file and get an advisory lock on it. The
		 * lock_fd is left open to keep the lock.
		 */
#ifdef HAVE_FCNTL_SETLK
#ifndef __CYGWIN32__
		if ((lock_fd = open(sock_path, O_WRONLY | O_NONBLOCK, 0666)) >= 0)
#else
		if ((lock_fd = open(sock_path, O_WRONLY | O_NONBLOCK | O_BINARY, 0666)) >= 0)
#endif
		{
			struct flock	lck;
			
			lck.l_whence = SEEK_SET; lck.l_start = lck.l_len = 0;
			lck.l_type = F_WRLCK;
			if (fcntl(lock_fd, F_SETLK, &lck) != 0)
				TPRINTF(TRACE_VERBOSE, "flock error for %s", sock_path);
		}
#endif /* HAVE_FCNTL_SETLK */
	}

	listen(fd, SOMAXCONN);

	/*
	 * MS: I took this code from Dillon's version.  It makes the listening
	 * port non-blocking.  That is not necessary (and may tickle kernel
	 * bugs).
	 *
	 * fcntl(fd, F_SETFD, 1); fcntl(fd, F_SETFL, FNDELAY);
	 */

	*fdP = fd;
	if (family == AF_UNIX)
		chmod(sock_path, 0777);
	return STATUS_OK;
}

/*
 * StreamConnection -- create a new connection with client using
 *		server port.
 *
 * ASSUME: that this doesn't need to be non-blocking because
 *		the Postmaster uses select() to tell when the server master
 *		socket is ready for accept().
 *
 * RETURNS: STATUS_OK or STATUS_ERROR
 */
int
StreamConnection(int server_fd, Port *port)
{
	SOCKET_SIZE_TYPE	addrlen;

	/* accept connection (and fill in the client (remote) address) */
	addrlen = sizeof(port->raddr);
	if ((port->sock = accept(server_fd,
							 (struct sockaddr *) & port->raddr,
							 &addrlen)) < 0)
	{
		elog(ERROR, "postmaster: StreamConnection: accept: %m");
		return STATUS_ERROR;
	}

	/* fill in the server (local) address */
	addrlen = sizeof(port->laddr);
	if (getsockname(port->sock, (struct sockaddr *) & port->laddr,
					&addrlen) < 0)
	{
		elog(ERROR, "postmaster: StreamConnection: getsockname: %m");
		return STATUS_ERROR;
	}

	/* select TCP_NODELAY option if it's a TCP connection */
	if (port->laddr.sa.sa_family == AF_INET)
	{
		struct protoent *pe;
		int			on = 1;

		pe = getprotobyname("TCP");
		if (pe == NULL)
		{
			elog(ERROR, "postmaster: getprotobyname failed");
			return STATUS_ERROR;
		}
		if (setsockopt(port->sock, pe->p_proto, TCP_NODELAY,
					   &on, sizeof(on)) < 0)
		{
			elog(ERROR, "postmaster: setsockopt failed: %m");
			return STATUS_ERROR;
		}
	}

	/* reset to non-blocking */
	fcntl(port->sock, F_SETFL, 1);

	return STATUS_OK;
}

/*
 * StreamClose -- close a client/backend connection
 */
void
StreamClose(int sock)
{
	close(sock);
}


/* --------------------------------
 * Low-level I/O routines begin here.
 *
 * These routines communicate with a frontend client across a connection
 * already established by the preceding routines.
 * --------------------------------
 */


/* --------------------------------
 *		pq_recvbuf - load some bytes into the input buffer
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
static int
pq_recvbuf(void)
{
	if (PqRecvPointer > 0)
	{
		if (PqRecvLength > PqRecvPointer)
		{
			/* still some unread data, left-justify it in the buffer */
			memmove(PqRecvBuffer, PqRecvBuffer+PqRecvPointer,
					PqRecvLength-PqRecvPointer);
			PqRecvLength -= PqRecvPointer;
			PqRecvPointer = 0;
		}
		else
			PqRecvLength = PqRecvPointer = 0;
	}

	/* Can fill buffer from PqRecvLength and upwards */
	for (;;)
	{
		int r = recv(MyProcPort->sock, PqRecvBuffer + PqRecvLength,
					 PQ_BUFFER_SIZE - PqRecvLength, 0);
		if (r < 0)
		{
			if (errno == EINTR)
				continue;		/* Ok if interrupted */
			/* We would like to use elog() here, but dare not because elog
			 * tries to write to the client, which will cause problems
			 * if we have a hard communications failure ...
			 * So just write the message to the postmaster log.
			 */
			fprintf(stderr, "pq_recvbuf: recv() failed, errno=%d\n", errno);
			return EOF;
		}
		if (r == 0)
		{
			/* as above, elog not safe */
			fprintf(stderr, "pq_recvbuf: unexpected EOF on client connection\n");
			return EOF;
		}
		/* r contains number of bytes read, so just incr length */
		PqRecvLength += r;
		return 0;
	}
}

/* --------------------------------
 *		pq_getbyte	- get a single byte from connection, or return EOF
 * --------------------------------
 */
static int
pq_getbyte(void)
{
	while (PqRecvPointer >= PqRecvLength)
	{
		if (pq_recvbuf())		/* If nothing in buffer, then recv some */
			return EOF;			/* Failed to recv data */
	}
	return PqRecvBuffer[PqRecvPointer++];
}

/* --------------------------------
 *		pq_peekbyte		- peek at next byte from connection
 *
 *	 Same as pq_getbyte() except we don't advance the pointer.
 * --------------------------------
 */
int
pq_peekbyte(void)
{
	while (PqRecvPointer >= PqRecvLength)
	{
		if (pq_recvbuf())		/* If nothing in buffer, then recv some */
			return EOF;			/* Failed to recv data */
	}
	return PqRecvBuffer[PqRecvPointer];
}

/* --------------------------------
 *		pq_getbytes		- get a known number of bytes from connection
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
int
pq_getbytes(char *s, size_t len)
{
	size_t amount;

	while (len > 0)
	{
		while (PqRecvPointer >= PqRecvLength)
		{
			if (pq_recvbuf())	/* If nothing in buffer, then recv some */
				return EOF;		/* Failed to recv data */
		}
		amount = PqRecvLength - PqRecvPointer;
		if (amount > len)
			amount = len;
		memcpy(s, PqRecvBuffer + PqRecvPointer, amount);
		PqRecvPointer += amount;
		s += amount;
		len -= amount;
	}
	return 0;
}

/* --------------------------------
 *		pq_getstring	- get a null terminated string from connection
 *
 *		NOTE: this routine does not do any MULTIBYTE conversion,
 *		even though it is presumably useful only for text, because
 *		no code in this module should depend on MULTIBYTE mode.
 *		See pq_getstr in pqformat.c for that.
 *
 *		FIXME: we ought to use an expansible StringInfo buffer,
 *		rather than dropping data if the message is too long.
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
int
pq_getstring(char *s, size_t len)
{
	int			c;

	/*
	 * Keep on reading until we get the terminating '\0',
	 * discarding any bytes we don't have room for.
	 */

	while ((c = pq_getbyte()) != EOF && c != '\0')
	{
		if (len > 1)
		{
			*s++ = c;
			len--;
		}
	}

	*s = '\0';

	if (c == EOF)
		return EOF;

	return 0;
}


/* --------------------------------
 *		pq_putbytes		- send bytes to connection (not flushed until pq_flush)
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
int
pq_putbytes(const char *s, size_t len)
{
	size_t amount;

	while (len > 0)
	{
		if (PqSendPointer >= PQ_BUFFER_SIZE)
			if (pq_flush())		/* If buffer is full, then flush it out */
				return EOF;
		amount = PQ_BUFFER_SIZE - PqSendPointer;
		if (amount > len)
			amount = len;
		memcpy(PqSendBuffer + PqSendPointer, s, amount);
		PqSendPointer += amount;
		s += amount;
		len -= amount;
	}
	return 0;
}

/* --------------------------------
 *		pq_flush		- flush pending output
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
int
pq_flush(void)
{
	unsigned char *bufptr = PqSendBuffer;
	unsigned char *bufend = PqSendBuffer + PqSendPointer;

	while (bufptr < bufend)
	{
		int r = send(MyProcPort->sock, bufptr, bufend - bufptr, 0);
		if (r <= 0)
		{
			if (errno == EINTR)
				continue;		/* Ok if we were interrupted */
			/* We would like to use elog() here, but cannot because elog
			 * tries to write to the client, which would cause a recursive
			 * flush attempt!  So just write it out to the postmaster log.
			 */
			fprintf(stderr, "pq_flush: send() failed, errno %d\n", errno);
			/* We drop the buffered data anyway so that processing
			 * can continue, even though we'll probably quit soon.
			 */
			PqSendPointer = 0;
			return EOF;
		}
		bufptr += r;
	}
	PqSendPointer = 0;
	return 0;
}


/* --------------------------------
 * Message-level I/O routines begin here.
 *
 * These routines understand about COPY OUT protocol.
 * --------------------------------
 */


/* --------------------------------
 *		pq_putmessage	- send a normal message (suppressed in COPY OUT mode)
 *
 *		If msgtype is not '\0', it is a message type code to place before
 *		the message body (len counts only the body size!).
 *		If msgtype is '\0', then the buffer already includes the type code.
 *
 *		All normal messages are suppressed while COPY OUT is in progress.
 *		(In practice only NOTICE messages might get emitted then; dropping
 *		them is annoying, but at least they will still appear in the
 *		postmaster log.)
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
int
pq_putmessage(char msgtype, const char *s, size_t len)
{
	if (DoingCopyOut)
		return 0;
	if (msgtype)
		if (pq_putbytes(&msgtype, 1))
			return EOF;
	return pq_putbytes(s, len);
}

/* --------------------------------
 *		pq_startcopyout	- inform libpq that a COPY OUT transfer is beginning
 * --------------------------------
 */
void
pq_startcopyout(void)
{
	DoingCopyOut = true;
}

/* --------------------------------
 *		pq_endcopyout	- end a COPY OUT transfer
 *
 *		If errorAbort is indicated, we are aborting a COPY OUT due to an error,
 *		and must send a terminator line.  Since a partial data line might have
 *		been emitted, send a couple of newlines first (the first one could
 *		get absorbed by a backslash...)
 * --------------------------------
 */
void
pq_endcopyout(bool errorAbort)
{
	if (! DoingCopyOut)
		return;
	if (errorAbort)
		pq_putbytes("\n\n\\.\n", 5);
	/* in non-error case, copy.c will have emitted the terminator line */
	DoingCopyOut = false;
}
