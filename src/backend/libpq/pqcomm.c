/*-------------------------------------------------------------------------
 *
 * pqcomm.c--
 *	  Communication functions between the Frontend and the Backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *  $Id: pqcomm.c,v 1.62 1999/01/17 03:10:23 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *              pq_init                 - initialize libpq
 *		pq_getport		- return the PGPORT setting
 *		pq_close		- close input / output connections
 *		pq_flush		- flush pending output
 *		pq_getstr		- get a null terminated string from connection
 *              pq_getchar              - get 1 character from connection
 *              pq_peekchar             - peek at first character in connection
 *		pq_getnchar		- get n characters from connection, and null-terminate
 *		pq_getint		- get an integer from connection
 *              pq_putchar              - send 1 character to connection
 *		pq_putstr		- send a null terminated string to connection
 *		pq_putnchar		- send n characters to connection
 *		pq_putint		- send an integer to connection
 *		pq_putncharlen		- send n characters to connection
 *					  (also send an int header indicating
 *					   the length)
 *		pq_getinaddr	- initialize address from host and port number
 *		pq_getinserv	- initialize address from host and service name
 *
 *              StreamDoUnlink          - Shutdown UNIX socket connectioin
 *              StreamServerPort        - Open sock stream
 *              StreamConnection        - Create new connection with client
 *              StreamClose             - Close a client/backend connection
 * 
 * NOTES
 *              Frontend is now completey in interfaces/libpq, and no 
 *              functions from this file is used.
 *
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

#if defined(linux)
#ifndef SOMAXCONN
#define SOMAXCONN 5				/* from Linux listen(2) man page */
#endif	 /* SOMAXCONN */
#endif	 /* linux */

#include "miscadmin.h"
#include "libpq/pqsignal.h"
#include "libpq/auth.h"
#include "libpq/libpq.h"		/* where the declarations go */
#include "storage/ipc.h"
#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif
#include "utils/trace.h"

extern FILE * debug_port; /* in util.c */

/* --------------------------------
 *		pq_init - open portal file descriptors
 * --------------------------------
 */
void
pq_init(int fd)
{
	PQnotifies_init();
	if (getenv("LIBPQ_DEBUG"))
	  debug_port = stderr;
}

/* -------------------------
 *	 pq_getchar()
 *
 *	 get a character from the input file,
 *
 */

int
pq_getchar(void)
{
	char c;

	while (recv(MyProcPort->sock, &c, 1, 0) != 1) {
	    if (errno != EINTR)
			return EOF; /* Not interrupted, so something went wrong */
	}
	  
	return c;
}

/*
 * --------------------------------
 *              pq_peekchar - get 1 character from connection, but leave it in the stream
 */
int
pq_peekchar(void) {
	char c;

	while (recv(MyProcPort->sock, &c, 1, MSG_PEEK) != 1) {
	    if (errno != EINTR)
			return EOF; /* Not interrupted, so something went wrong */
	}

	return c;
}
  


/* --------------------------------
 *		pq_getport - return the PGPORT setting
 * --------------------------------
 */
int
pq_getport()
{
	char	   *envport = getenv("PGPORT");

	if (envport)
		return atoi(envport);
	return atoi(DEF_PGPORT);
}

/* --------------------------------
 *		pq_close - close input / output connections
 * --------------------------------
 */
void
pq_close()
{
        close(MyProcPort->sock);
	PQnotifies_init();
}

/* --------------------------------
 *		pq_flush - flush pending output
 * --------------------------------
 */
void
pq_flush()
{
  /* Not supported/required? */
}

/* --------------------------------
 *		pq_getstr - get a null terminated string from connection
 * --------------------------------
 */
int
pq_getstr(char *s, int maxlen)
{
	int			c;
#ifdef MULTIBYTE
	char	   *p;
#endif

	c = pqGetString(s, maxlen);

#ifdef MULTIBYTE
	p = (char*) pg_client_to_server((unsigned char *) s, maxlen);
	if (s != p)					/* actual conversion has been done? */
		strcpy(s, p);
#endif

	return c;
}

/* --------------------------------
 *		pq_getnchar - get n characters from connection, and null terminate
 * --------------------------------
 */
int
pq_getnchar(char *s, int off, int maxlen)
{
        int r = pqGetNBytes(s + off, maxlen);
	s[off+maxlen] = '\0';
	return r;
}

/* --------------------------------
 *		pq_getint - get an integer from connection
 *	 we receive an integer a byte at a type and reconstruct it so that
 *	 machines with different ENDIAN representations can talk to each
 *	 other
 * --------------------------------
 */
int
pq_getint(int b)
{
	int			n,
				status = 1;

	/*
	 * mjl: Seems inconsisten w/ return value of pq_putint (void). Also,
	 * EOF is a valid return value for an int! XXX
	 */

	switch (b)
	{
		case 1:
			status = ((n = pq_getchar()) == EOF);
			break;
		case 2:
			status = pqGetShort(&n);
			break;
		case 4:
			status = pqGetLong(&n);
			break;
		default:
			fprintf(stderr, "** Unsupported size %d\n", b);
	}

	if (status)
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"FATAL: pq_getint failed: errno=%d\n", errno);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		n = 0;
	}

	return n;
}

/* --------------------------------
 *		pq_putstr - send a null terminated string to connection
 * --------------------------------
 */
void
pq_putstr(char *s)
{
#ifdef MULTIBYTE
	unsigned char *p;

	p = pg_server_to_client(s, strlen(s));
	if (pqPutString(p))
#else
	if (pqPutString(s))
#endif
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"FATAL: pq_putstr: fputs() failed: errno=%d\n", errno);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
	}
}

/* --------------------------------
 *		pq_putnchar - send n characters to connection
 * --------------------------------
 */
void
pq_putnchar(char *s, int n)
{
	if (pqPutNBytes(s, n))
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"FATAL: pq_putnchar: pqPutNBytes() failed: errno=%d\n",
				errno);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
	}
}

/* --------------------------------
 *		pq_putint - send an integer to connection
 *	 we chop an integer into bytes and send individual bytes
 *	 machines with different ENDIAN representations can still talk to each
 *	 other
 * --------------------------------
 */
void
pq_putint(int i, int b)
{
	int			status;

	status = 1;
	switch (b)
	{
		case 1:
			status = (pq_putchar(i) == EOF);
			break;
		case 2:
			status = pqPutShort(i);
			break;
		case 4:
			status = pqPutLong(i);
			break;
		default:
			fprintf(stderr, "** Unsupported size %d\n", b);
	}

	if (status)
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"FATAL: pq_putint failed: errno=%d\n", errno);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
	}
}

/* --------------------------------
 *		pq_getinaddr - initialize address from host and port number
 * --------------------------------
 */
int
pq_getinaddr(struct sockaddr_in * sin,
			 char *host,
			 int port)
{
	struct hostent *hs;

	MemSet((char *) sin, 0, sizeof(*sin));

	if (host)
	{
		if (*host >= '0' && *host <= '9')
			sin->sin_addr.s_addr = inet_addr(host);
		else
		{
			if (!(hs = gethostbyname(host)))
			{
				perror(host);
				return 1;
			}
			if (hs->h_addrtype != AF_INET)
			{
				snprintf(PQerrormsg, ERROR_MSG_LENGTH,
						"FATAL: pq_getinaddr: %s not on Internet\n",
						host);
				fputs(PQerrormsg, stderr);
				pqdebug("%s", PQerrormsg);
				return 1;
			}
			memmove((char *) &sin->sin_addr,
					hs->h_addr,
					hs->h_length);
		}
	}
	sin->sin_family = AF_INET;
	sin->sin_port = htons(port);
	return 0;
}

/* --------------------------------
 *		pq_getinserv - initialize address from host and servive name
 * --------------------------------
 */
int
pq_getinserv(struct sockaddr_in * sin, char *host, char *serv)
{
	struct servent *ss;

	if (*serv >= '0' && *serv <= '9')
		return pq_getinaddr(sin, host, atoi(serv));
	if (!(ss = getservbyname(serv, NULL)))
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"FATAL: pq_getinserv: unknown service: %s\n",
				serv);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return 1;
	}
	return pq_getinaddr(sin, host, ntohs(ss->s_port));
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
void
StreamDoUnlink()
{
	Assert(sock_path[0]);
	unlink(sock_path);
}

/*
 * StreamServerPort -- open a sock stream "listening" port.
 *
 * This initializes the Postmaster's connection
 *		accepting port.
 *
 * ASSUME: that this doesn't need to be non-blocking because
 *		the Postmaster uses select() to tell when the socket
 *		is ready.
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
		if ((lock_fd = open(sock_path, O_WRONLY | O_NONBLOCK, 0666)) >= 0)
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
		if ((lock_fd = open(sock_path, O_WRONLY | O_NONBLOCK, 0666)) >= 0)
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
 * This one should be non-blocking.
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
			elog(ERROR, "postmaster: setsockopt failed");
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

#ifdef MULTIBYTE
void
pq_putncharlen(char *s, int n)
{
	unsigned char *p;
	int			len;

	p = pg_server_to_client(s, n);
	len = strlen(p);
	pq_putint(len, sizeof(int));
	pq_putnchar(p, len);
}

#endif


/* 
 * Act like the stdio putc() function. Write one character
 * to the stream. Return this character, or EOF on error.
 */
int pq_putchar(char c) 
{
  char isDone = 0;

  do {
    if (send(MyProcPort->sock, &c, 1, 0) != 1) {
      if (errno != EINTR) 
	return EOF; /* Anything other than interrupt is error! */
    }
    else
      isDone = 1; /* Done if we sent one char */
  } while (!isDone);
  return c;
}



