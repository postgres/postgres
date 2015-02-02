/*-------------------------------------------------------------------------
 *
 * pqcomm.c
 *	  Communication functions between the Frontend and the Backend
 *
 * These routines handle the low-level details of communication between
 * frontend and backend.  They just shove data across the communication
 * channel, and are ignorant of the semantics of the data --- or would be,
 * except for major brain damage in the design of the old COPY OUT protocol.
 * Unfortunately, COPY OUT was designed to commandeer the communication
 * channel (it just transfers data without wrapping it into messages).
 * No other messages can be sent while COPY OUT is in progress; and if the
 * copy is aborted by an ereport(ERROR), we need to close out the copy so that
 * the frontend gets back into sync.  Therefore, these routines have to be
 * aware of COPY OUT state.  (New COPY-OUT is message-based and does *not*
 * set the DoingCopyOut flag.)
 *
 * NOTE: generally, it's a bad idea to emit outgoing messages directly with
 * pq_putbytes(), especially if the message would require multiple calls
 * to send.  Instead, use the routines in pqformat.c to construct the message
 * in a buffer and then emit it in one call to pq_putmessage.  This ensures
 * that the channel will not be clogged by an incomplete message if execution
 * is aborted by ereport(ERROR) partway through the message.  The only
 * non-libpq code that should call pq_putbytes directly is old-style COPY OUT.
 *
 * At one time, libpq was shared between frontend and backend, but now
 * the backend's "backend/libpq" is quite separate from "interfaces/libpq".
 * All that remains is similarities of names to trap the unwary...
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/libpq/pqcomm.c
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
 *		TouchSocketFiles	- Protect socket files against /tmp cleaners
 *		pq_init			- initialize libpq at backend startup
 *		pq_comm_reset	- reset libpq during error recovery
 *		pq_close		- shutdown libpq at backend exit
 *
 * low-level I/O:
 *		pq_getbytes		- get a known number of bytes from connection
 *		pq_getstring	- get a null terminated string from connection
 *		pq_getmessage	- get a message with length word from connection
 *		pq_getbyte		- get next byte from connection
 *		pq_peekbyte		- peek at next byte from connection
 *		pq_putbytes		- send bytes to connection (not flushed until pq_flush)
 *		pq_flush		- flush pending output
 *		pq_flush_if_writable - flush pending output if writable without blocking
 *		pq_getbyte_if_available - get a byte if available without blocking
 *
 * message-level I/O (and old-style-COPY-OUT cruft):
 *		pq_putmessage	- send a normal message (suppressed in COPY OUT mode)
 *		pq_putmessage_noblock - buffer a normal message (suppressed in COPY OUT)
 *		pq_startcopyout - inform libpq that a COPY OUT transfer is beginning
 *		pq_endcopyout	- end a COPY OUT transfer
 *
 *------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <fcntl.h>
#include <grp.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#ifdef HAVE_UTIME_H
#include <utime.h>
#endif
#ifdef WIN32_ONLY_COMPILER		/* mstcpip.h is missing on mingw */
#include <mstcpip.h>
#endif

#include "libpq/ip.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/memutils.h"

/*
 * Configuration options
 */
int			Unix_socket_permissions;
char	   *Unix_socket_group;


/* Where the Unix socket files are (list of palloc'd strings) */
static List *sock_paths = NIL;


/*
 * Buffers for low-level I/O.
 *
 * The receive buffer is fixed size. Send buffer is usually 8k, but can be
 * enlarged by pq_putmessage_noblock() if the message doesn't fit otherwise.
 */

#define PQ_SEND_BUFFER_SIZE 8192
#define PQ_RECV_BUFFER_SIZE 8192

static char *PqSendBuffer;
static int	PqSendBufferSize;	/* Size send buffer */
static int	PqSendPointer;		/* Next index to store a byte in PqSendBuffer */
static int	PqSendStart;		/* Next index to send a byte in PqSendBuffer */

static char PqRecvBuffer[PQ_RECV_BUFFER_SIZE];
static int	PqRecvPointer;		/* Next index to read a byte from PqRecvBuffer */
static int	PqRecvLength;		/* End of data available in PqRecvBuffer */

/*
 * Message status
 */
static bool PqCommBusy;			/* busy sending data to the client */
static bool PqCommReadingMsg;	/* in the middle of reading a message */
static bool DoingCopyOut;		/* in old-protocol COPY OUT processing */


/* Internal functions */
static void pq_close(int code, Datum arg);
static int	internal_putbytes(const char *s, size_t len);
static int	internal_flush(void);
static void pq_set_nonblocking(bool nonblocking);

#ifdef HAVE_UNIX_SOCKETS
static int	Lock_AF_UNIX(char *unixSocketDir, char *unixSocketPath);
static int	Setup_AF_UNIX(char *sock_path);
#endif   /* HAVE_UNIX_SOCKETS */


/* --------------------------------
 *		pq_init - initialize libpq at backend startup
 * --------------------------------
 */
void
pq_init(void)
{
	PqSendBufferSize = PQ_SEND_BUFFER_SIZE;
	PqSendBuffer = MemoryContextAlloc(TopMemoryContext, PqSendBufferSize);
	PqSendPointer = PqSendStart = PqRecvPointer = PqRecvLength = 0;
	PqCommBusy = false;
	PqCommReadingMsg = false;
	DoingCopyOut = false;
	on_proc_exit(pq_close, 0);
}

/* --------------------------------
 *		pq_comm_reset - reset libpq during error recovery
 *
 * This is called from error recovery at the outer idle loop.  It's
 * just to get us out of trouble if we somehow manage to elog() from
 * inside a pqcomm.c routine (which ideally will never happen, but...)
 * --------------------------------
 */
void
pq_comm_reset(void)
{
	/* Do not throw away pending data, but do reset the busy flag */
	PqCommBusy = false;
	/* We can abort any old-style COPY OUT, too */
	pq_endcopyout(true);
}

/* --------------------------------
 *		pq_close - shutdown libpq at backend exit
 *
 * Note: in a standalone backend MyProcPort will be null,
 * don't crash during exit...
 * --------------------------------
 */
static void
pq_close(int code, Datum arg)
{
	if (MyProcPort != NULL)
	{
#if defined(ENABLE_GSS) || defined(ENABLE_SSPI)
#ifdef ENABLE_GSS
		OM_uint32	min_s;

		/* Shutdown GSSAPI layer */
		if (MyProcPort->gss->ctx != GSS_C_NO_CONTEXT)
			gss_delete_sec_context(&min_s, &MyProcPort->gss->ctx, NULL);

		if (MyProcPort->gss->cred != GSS_C_NO_CREDENTIAL)
			gss_release_cred(&min_s, &MyProcPort->gss->cred);
#endif   /* ENABLE_GSS */
		/* GSS and SSPI share the port->gss struct */

		free(MyProcPort->gss);
#endif   /* ENABLE_GSS || ENABLE_SSPI */

		/* Cleanly shut down SSL layer */
		secure_close(MyProcPort);

		/*
		 * Formerly we did an explicit close() here, but it seems better to
		 * leave the socket open until the process dies.  This allows clients
		 * to perform a "synchronous close" if they care --- wait till the
		 * transport layer reports connection closure, and you can be sure the
		 * backend has exited.
		 *
		 * We do set sock to PGINVALID_SOCKET to prevent any further I/O,
		 * though.
		 */
		MyProcPort->sock = PGINVALID_SOCKET;
	}
}



/*
 * Streams -- wrapper around Unix socket system calls
 *
 *
 *		Stream functions are used for vanilla TCP connection protocol.
 */


/* StreamDoUnlink()
 * Shutdown routine for backend connection
 * If any Unix sockets are used for communication, explicitly close them.
 */
#ifdef HAVE_UNIX_SOCKETS
static void
StreamDoUnlink(int code, Datum arg)
{
	ListCell   *l;

	/* Loop through all created sockets... */
	foreach(l, sock_paths)
	{
		char	   *sock_path = (char *) lfirst(l);

		unlink(sock_path);
	}
	/* Since we're about to exit, no need to reclaim storage */
	sock_paths = NIL;
}
#endif   /* HAVE_UNIX_SOCKETS */

/*
 * StreamServerPort -- open a "listening" port to accept connections.
 *
 * family should be AF_UNIX or AF_UNSPEC; portNumber is the port number.
 * For AF_UNIX ports, hostName should be NULL and unixSocketDir must be
 * specified.  For TCP ports, hostName is either NULL for all interfaces or
 * the interface to listen on, and unixSocketDir is ignored (can be NULL).
 *
 * Successfully opened sockets are added to the ListenSocket[] array (of
 * length MaxListen), at the first position that isn't PGINVALID_SOCKET.
 *
 * RETURNS: STATUS_OK or STATUS_ERROR
 */

int
StreamServerPort(int family, char *hostName, unsigned short portNumber,
				 char *unixSocketDir,
				 pgsocket ListenSocket[], int MaxListen)
{
	pgsocket	fd;
	int			err;
	int			maxconn;
	int			ret;
	char		portNumberStr[32];
	const char *familyDesc;
	char		familyDescBuf[64];
	char	   *service;
	struct addrinfo *addrs = NULL,
			   *addr;
	struct addrinfo hint;
	int			listen_index = 0;
	int			added = 0;

#ifdef HAVE_UNIX_SOCKETS
	char		unixSocketPath[MAXPGPATH];
#endif
#if !defined(WIN32) || defined(IPV6_V6ONLY)
	int			one = 1;
#endif

	/* Initialize hint structure */
	MemSet(&hint, 0, sizeof(hint));
	hint.ai_family = family;
	hint.ai_flags = AI_PASSIVE;
	hint.ai_socktype = SOCK_STREAM;

#ifdef HAVE_UNIX_SOCKETS
	if (family == AF_UNIX)
	{
		/*
		 * Create unixSocketPath from portNumber and unixSocketDir and lock
		 * that file path
		 */
		UNIXSOCK_PATH(unixSocketPath, portNumber, unixSocketDir);
		if (strlen(unixSocketPath) >= UNIXSOCK_PATH_BUFLEN)
		{
			ereport(LOG,
					(errmsg("Unix-domain socket path \"%s\" is too long (maximum %d bytes)",
							unixSocketPath,
							(int) (UNIXSOCK_PATH_BUFLEN - 1))));
			return STATUS_ERROR;
		}
		if (Lock_AF_UNIX(unixSocketDir, unixSocketPath) != STATUS_OK)
			return STATUS_ERROR;
		service = unixSocketPath;
	}
	else
#endif   /* HAVE_UNIX_SOCKETS */
	{
		snprintf(portNumberStr, sizeof(portNumberStr), "%d", portNumber);
		service = portNumberStr;
	}

	ret = pg_getaddrinfo_all(hostName, service, &hint, &addrs);
	if (ret || !addrs)
	{
		if (hostName)
			ereport(LOG,
					(errmsg("could not translate host name \"%s\", service \"%s\" to address: %s",
							hostName, service, gai_strerror(ret))));
		else
			ereport(LOG,
				 (errmsg("could not translate service \"%s\" to address: %s",
						 service, gai_strerror(ret))));
		if (addrs)
			pg_freeaddrinfo_all(hint.ai_family, addrs);
		return STATUS_ERROR;
	}

	for (addr = addrs; addr; addr = addr->ai_next)
	{
		if (!IS_AF_UNIX(family) && IS_AF_UNIX(addr->ai_family))
		{
			/*
			 * Only set up a unix domain socket when they really asked for it.
			 * The service/port is different in that case.
			 */
			continue;
		}

		/* See if there is still room to add 1 more socket. */
		for (; listen_index < MaxListen; listen_index++)
		{
			if (ListenSocket[listen_index] == PGINVALID_SOCKET)
				break;
		}
		if (listen_index >= MaxListen)
		{
			ereport(LOG,
					(errmsg("could not bind to all requested addresses: MAXLISTEN (%d) exceeded",
							MaxListen)));
			break;
		}

		/* set up family name for possible error messages */
		switch (addr->ai_family)
		{
			case AF_INET:
				familyDesc = _("IPv4");
				break;
#ifdef HAVE_IPV6
			case AF_INET6:
				familyDesc = _("IPv6");
				break;
#endif
#ifdef HAVE_UNIX_SOCKETS
			case AF_UNIX:
				familyDesc = _("Unix");
				break;
#endif
			default:
				snprintf(familyDescBuf, sizeof(familyDescBuf),
						 _("unrecognized address family %d"),
						 addr->ai_family);
				familyDesc = familyDescBuf;
				break;
		}

		if ((fd = socket(addr->ai_family, SOCK_STREAM, 0)) == PGINVALID_SOCKET)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
			/* translator: %s is IPv4, IPv6, or Unix */
					 errmsg("could not create %s socket: %m",
							familyDesc)));
			continue;
		}

#ifndef WIN32

		/*
		 * Without the SO_REUSEADDR flag, a new postmaster can't be started
		 * right away after a stop or crash, giving "address already in use"
		 * error on TCP ports.
		 *
		 * On win32, however, this behavior only happens if the
		 * SO_EXLUSIVEADDRUSE is set. With SO_REUSEADDR, win32 allows multiple
		 * servers to listen on the same address, resulting in unpredictable
		 * behavior. With no flags at all, win32 behaves as Unix with
		 * SO_REUSEADDR.
		 */
		if (!IS_AF_UNIX(addr->ai_family))
		{
			if ((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
							(char *) &one, sizeof(one))) == -1)
			{
				ereport(LOG,
						(errcode_for_socket_access(),
						 errmsg("setsockopt(SO_REUSEADDR) failed: %m")));
				closesocket(fd);
				continue;
			}
		}
#endif

#ifdef IPV6_V6ONLY
		if (addr->ai_family == AF_INET6)
		{
			if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
						   (char *) &one, sizeof(one)) == -1)
			{
				ereport(LOG,
						(errcode_for_socket_access(),
						 errmsg("setsockopt(IPV6_V6ONLY) failed: %m")));
				closesocket(fd);
				continue;
			}
		}
#endif

		/*
		 * Note: This might fail on some OS's, like Linux older than
		 * 2.4.21-pre3, that don't have the IPV6_V6ONLY socket option, and map
		 * ipv4 addresses to ipv6.  It will show ::ffff:ipv4 for all ipv4
		 * connections.
		 */
		err = bind(fd, addr->ai_addr, addr->ai_addrlen);
		if (err < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
			/* translator: %s is IPv4, IPv6, or Unix */
					 errmsg("could not bind %s socket: %m",
							familyDesc),
					 (IS_AF_UNIX(addr->ai_family)) ?
				  errhint("Is another postmaster already running on port %d?"
						  " If not, remove socket file \"%s\" and retry.",
						  (int) portNumber, service) :
				  errhint("Is another postmaster already running on port %d?"
						  " If not, wait a few seconds and retry.",
						  (int) portNumber)));
			closesocket(fd);
			continue;
		}

#ifdef HAVE_UNIX_SOCKETS
		if (addr->ai_family == AF_UNIX)
		{
			if (Setup_AF_UNIX(service) != STATUS_OK)
			{
				closesocket(fd);
				break;
			}
		}
#endif

		/*
		 * Select appropriate accept-queue length limit.  PG_SOMAXCONN is only
		 * intended to provide a clamp on the request on platforms where an
		 * overly large request provokes a kernel error (are there any?).
		 */
		maxconn = MaxBackends * 2;
		if (maxconn > PG_SOMAXCONN)
			maxconn = PG_SOMAXCONN;

		err = listen(fd, maxconn);
		if (err < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
			/* translator: %s is IPv4, IPv6, or Unix */
					 errmsg("could not listen on %s socket: %m",
							familyDesc)));
			closesocket(fd);
			continue;
		}
		ListenSocket[listen_index] = fd;
		added++;
	}

	pg_freeaddrinfo_all(hint.ai_family, addrs);

	if (!added)
		return STATUS_ERROR;

	return STATUS_OK;
}


#ifdef HAVE_UNIX_SOCKETS

/*
 * Lock_AF_UNIX -- configure unix socket file path
 */
static int
Lock_AF_UNIX(char *unixSocketDir, char *unixSocketPath)
{
	/*
	 * Grab an interlock file associated with the socket file.
	 *
	 * Note: there are two reasons for using a socket lock file, rather than
	 * trying to interlock directly on the socket itself.  First, it's a lot
	 * more portable, and second, it lets us remove any pre-existing socket
	 * file without race conditions.
	 */
	CreateSocketLockFile(unixSocketPath, true, unixSocketDir);

	/*
	 * Once we have the interlock, we can safely delete any pre-existing
	 * socket file to avoid failure at bind() time.
	 */
	unlink(unixSocketPath);

	/*
	 * Arrange to unlink the socket file(s) at proc_exit.  If this is the
	 * first one, set up the on_proc_exit function to do it; then add this
	 * socket file to the list of files to unlink.
	 */
	if (sock_paths == NIL)
		on_proc_exit(StreamDoUnlink, 0);

	sock_paths = lappend(sock_paths, pstrdup(unixSocketPath));

	return STATUS_OK;
}


/*
 * Setup_AF_UNIX -- configure unix socket permissions
 */
static int
Setup_AF_UNIX(char *sock_path)
{
	/*
	 * Fix socket ownership/permission if requested.  Note we must do this
	 * before we listen() to avoid a window where unwanted connections could
	 * get accepted.
	 */
	Assert(Unix_socket_group);
	if (Unix_socket_group[0] != '\0')
	{
#ifdef WIN32
		elog(WARNING, "configuration item unix_socket_group is not supported on this platform");
#else
		char	   *endptr;
		unsigned long val;
		gid_t		gid;

		val = strtoul(Unix_socket_group, &endptr, 10);
		if (*endptr == '\0')
		{						/* numeric group id */
			gid = val;
		}
		else
		{						/* convert group name to id */
			struct group *gr;

			gr = getgrnam(Unix_socket_group);
			if (!gr)
			{
				ereport(LOG,
						(errmsg("group \"%s\" does not exist",
								Unix_socket_group)));
				return STATUS_ERROR;
			}
			gid = gr->gr_gid;
		}
		if (chown(sock_path, -1, gid) == -1)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not set group of file \"%s\": %m",
							sock_path)));
			return STATUS_ERROR;
		}
#endif
	}

	if (chmod(sock_path, Unix_socket_permissions) == -1)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not set permissions of file \"%s\": %m",
						sock_path)));
		return STATUS_ERROR;
	}
	return STATUS_OK;
}
#endif   /* HAVE_UNIX_SOCKETS */


/*
 * StreamConnection -- create a new connection with client using
 *		server port.  Set port->sock to the FD of the new connection.
 *
 * ASSUME: that this doesn't need to be non-blocking because
 *		the Postmaster uses select() to tell when the server master
 *		socket is ready for accept().
 *
 * RETURNS: STATUS_OK or STATUS_ERROR
 */
int
StreamConnection(pgsocket server_fd, Port *port)
{
	/* accept connection and fill in the client (remote) address */
	port->raddr.salen = sizeof(port->raddr.addr);
	if ((port->sock = accept(server_fd,
							 (struct sockaddr *) & port->raddr.addr,
							 &port->raddr.salen)) == PGINVALID_SOCKET)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not accept new connection: %m")));

		/*
		 * If accept() fails then postmaster.c will still see the server
		 * socket as read-ready, and will immediately try again.  To avoid
		 * uselessly sucking lots of CPU, delay a bit before trying again.
		 * (The most likely reason for failure is being out of kernel file
		 * table slots; we can do little except hope some will get freed up.)
		 */
		pg_usleep(100000L);		/* wait 0.1 sec */
		return STATUS_ERROR;
	}

#ifdef SCO_ACCEPT_BUG

	/*
	 * UnixWare 7+ and OpenServer 5.0.4 are known to have this bug, but it
	 * shouldn't hurt to catch it for all versions of those platforms.
	 */
	if (port->raddr.addr.ss_family == 0)
		port->raddr.addr.ss_family = AF_UNIX;
#endif

	/* fill in the server (local) address */
	port->laddr.salen = sizeof(port->laddr.addr);
	if (getsockname(port->sock,
					(struct sockaddr *) & port->laddr.addr,
					&port->laddr.salen) < 0)
	{
		elog(LOG, "getsockname() failed: %m");
		return STATUS_ERROR;
	}

	/* select NODELAY and KEEPALIVE options if it's a TCP connection */
	if (!IS_AF_UNIX(port->laddr.addr.ss_family))
	{
		int			on;

#ifdef	TCP_NODELAY
		on = 1;
		if (setsockopt(port->sock, IPPROTO_TCP, TCP_NODELAY,
					   (char *) &on, sizeof(on)) < 0)
		{
			elog(LOG, "setsockopt(TCP_NODELAY) failed: %m");
			return STATUS_ERROR;
		}
#endif
		on = 1;
		if (setsockopt(port->sock, SOL_SOCKET, SO_KEEPALIVE,
					   (char *) &on, sizeof(on)) < 0)
		{
			elog(LOG, "setsockopt(SO_KEEPALIVE) failed: %m");
			return STATUS_ERROR;
		}

#ifdef WIN32

		/*
		 * This is a Win32 socket optimization.  The ideal size is 32k.
		 * http://support.microsoft.com/kb/823764/EN-US/
		 */
		on = PQ_SEND_BUFFER_SIZE * 4;
		if (setsockopt(port->sock, SOL_SOCKET, SO_SNDBUF, (char *) &on,
					   sizeof(on)) < 0)
		{
			elog(LOG, "setsockopt(SO_SNDBUF) failed: %m");
			return STATUS_ERROR;
		}
#endif

		/*
		 * Also apply the current keepalive parameters.  If we fail to set a
		 * parameter, don't error out, because these aren't universally
		 * supported.  (Note: you might think we need to reset the GUC
		 * variables to 0 in such a case, but it's not necessary because the
		 * show hooks for these variables report the truth anyway.)
		 */
		(void) pq_setkeepalivesidle(tcp_keepalives_idle, port);
		(void) pq_setkeepalivesinterval(tcp_keepalives_interval, port);
		(void) pq_setkeepalivescount(tcp_keepalives_count, port);
	}

	return STATUS_OK;
}

/*
 * StreamClose -- close a client/backend connection
 *
 * NOTE: this is NOT used to terminate a session; it is just used to release
 * the file descriptor in a process that should no longer have the socket
 * open.  (For example, the postmaster calls this after passing ownership
 * of the connection to a child process.)  It is expected that someone else
 * still has the socket open.  So, we only want to close the descriptor,
 * we do NOT want to send anything to the far end.
 */
void
StreamClose(pgsocket sock)
{
	closesocket(sock);
}

/*
 * TouchSocketFiles -- mark socket files as recently accessed
 *
 * This routine should be called every so often to ensure that the socket
 * files have a recent mod date (ordinary operations on sockets usually won't
 * change the mod date).  That saves them from being removed by
 * overenthusiastic /tmp-directory-cleaner daemons.  (Another reason we should
 * never have put the socket file in /tmp...)
 */
void
TouchSocketFiles(void)
{
	ListCell   *l;

	/* Loop through all created sockets... */
	foreach(l, sock_paths)
	{
		char	   *sock_path = (char *) lfirst(l);

		/*
		 * utime() is POSIX standard, utimes() is a common alternative. If we
		 * have neither, there's no way to affect the mod or access time of
		 * the socket :-(
		 *
		 * In either path, we ignore errors; there's no point in complaining.
		 */
#ifdef HAVE_UTIME
		utime(sock_path, NULL);
#else							/* !HAVE_UTIME */
#ifdef HAVE_UTIMES
		utimes(sock_path, NULL);
#endif   /* HAVE_UTIMES */
#endif   /* HAVE_UTIME */
	}
}


/* --------------------------------
 * Low-level I/O routines begin here.
 *
 * These routines communicate with a frontend client across a connection
 * already established by the preceding routines.
 * --------------------------------
 */

/* --------------------------------
 *			  pq_set_nonblocking - set socket blocking/non-blocking
 *
 * Sets the socket non-blocking if nonblocking is TRUE, or sets it
 * blocking otherwise.
 * --------------------------------
 */
static void
pq_set_nonblocking(bool nonblocking)
{
	if (MyProcPort->noblock == nonblocking)
		return;

#ifdef WIN32
	pgwin32_noblock = nonblocking ? 1 : 0;
#else

	/*
	 * Use COMMERROR on failure, because ERROR would try to send the error to
	 * the client, which might require changing the mode again, leading to
	 * infinite recursion.
	 */
	if (nonblocking)
	{
		if (!pg_set_noblock(MyProcPort->sock))
			ereport(COMMERROR,
					(errmsg("could not set socket to nonblocking mode: %m")));
	}
	else
	{
		if (!pg_set_block(MyProcPort->sock))
			ereport(COMMERROR,
					(errmsg("could not set socket to blocking mode: %m")));
	}
#endif
	MyProcPort->noblock = nonblocking;
}

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
			memmove(PqRecvBuffer, PqRecvBuffer + PqRecvPointer,
					PqRecvLength - PqRecvPointer);
			PqRecvLength -= PqRecvPointer;
			PqRecvPointer = 0;
		}
		else
			PqRecvLength = PqRecvPointer = 0;
	}

	/* Ensure that we're in blocking mode */
	pq_set_nonblocking(false);

	/* Can fill buffer from PqRecvLength and upwards */
	for (;;)
	{
		int			r;

		r = secure_read(MyProcPort, PqRecvBuffer + PqRecvLength,
						PQ_RECV_BUFFER_SIZE - PqRecvLength);

		if (r < 0)
		{
			if (errno == EINTR)
				continue;		/* Ok if interrupted */

			/*
			 * Careful: an ereport() that tries to write to the client would
			 * cause recursion to here, leading to stack overflow and core
			 * dump!  This message must go *only* to the postmaster log.
			 */
			ereport(COMMERROR,
					(errcode_for_socket_access(),
					 errmsg("could not receive data from client: %m")));
			return EOF;
		}
		if (r == 0)
		{
			/*
			 * EOF detected.  We used to write a log message here, but it's
			 * better to expect the ultimate caller to do that.
			 */
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
int
pq_getbyte(void)
{
	Assert(PqCommReadingMsg);

	while (PqRecvPointer >= PqRecvLength)
	{
		if (pq_recvbuf())		/* If nothing in buffer, then recv some */
			return EOF;			/* Failed to recv data */
	}
	return (unsigned char) PqRecvBuffer[PqRecvPointer++];
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
	return (unsigned char) PqRecvBuffer[PqRecvPointer];
}

/* --------------------------------
 *		pq_getbyte_if_available - get a single byte from connection,
 *			if available
 *
 * The received byte is stored in *c. Returns 1 if a byte was read,
 * 0 if no data was available, or EOF if trouble.
 * --------------------------------
 */
int
pq_getbyte_if_available(unsigned char *c)
{
	int			r;

	Assert(PqCommReadingMsg);

	if (PqRecvPointer < PqRecvLength)
	{
		*c = PqRecvBuffer[PqRecvPointer++];
		return 1;
	}

	/* Put the socket into non-blocking mode */
	pq_set_nonblocking(true);

	r = secure_read(MyProcPort, c, 1);
	if (r < 0)
	{
		/*
		 * Ok if no data available without blocking or interrupted (though
		 * EINTR really shouldn't happen with a non-blocking socket). Report
		 * other errors.
		 */
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			r = 0;
		else
		{
			/*
			 * Careful: an ereport() that tries to write to the client would
			 * cause recursion to here, leading to stack overflow and core
			 * dump!  This message must go *only* to the postmaster log.
			 */
			ereport(COMMERROR,
					(errcode_for_socket_access(),
					 errmsg("could not receive data from client: %m")));
			r = EOF;
		}
	}
	else if (r == 0)
	{
		/* EOF detected */
		r = EOF;
	}

	return r;
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
	size_t		amount;

	Assert(PqCommReadingMsg);

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
 *		pq_discardbytes		- throw away a known number of bytes
 *
 *		same as pq_getbytes except we do not copy the data to anyplace.
 *		this is used for resynchronizing after read errors.
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
static int
pq_discardbytes(size_t len)
{
	size_t		amount;

	Assert(PqCommReadingMsg);

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
		PqRecvPointer += amount;
		len -= amount;
	}
	return 0;
}

/* --------------------------------
 *		pq_getstring	- get a null terminated string from connection
 *
 *		The return value is placed in an expansible StringInfo, which has
 *		already been initialized by the caller.
 *
 *		This is used only for dealing with old-protocol clients.  The idea
 *		is to produce a StringInfo that looks the same as we would get from
 *		pq_getmessage() with a newer client; we will then process it with
 *		pq_getmsgstring.  Therefore, no character set conversion is done here,
 *		even though this is presumably useful only for text.
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
int
pq_getstring(StringInfo s)
{
	int			i;

	Assert(PqCommReadingMsg);

	resetStringInfo(s);

	/* Read until we get the terminating '\0' */
	for (;;)
	{
		while (PqRecvPointer >= PqRecvLength)
		{
			if (pq_recvbuf())	/* If nothing in buffer, then recv some */
				return EOF;		/* Failed to recv data */
		}

		for (i = PqRecvPointer; i < PqRecvLength; i++)
		{
			if (PqRecvBuffer[i] == '\0')
			{
				/* include the '\0' in the copy */
				appendBinaryStringInfo(s, PqRecvBuffer + PqRecvPointer,
									   i - PqRecvPointer + 1);
				PqRecvPointer = i + 1;	/* advance past \0 */
				return 0;
			}
		}

		/* If we're here we haven't got the \0 in the buffer yet. */
		appendBinaryStringInfo(s, PqRecvBuffer + PqRecvPointer,
							   PqRecvLength - PqRecvPointer);
		PqRecvPointer = PqRecvLength;
	}
}


/* --------------------------------
 *		pq_startmsgread	- begin reading a message from the client.
 *
 *		This must be called before any of the pq_get* functions.
 * --------------------------------
 */
void
pq_startmsgread(void)
{
	/*
	 * There shouldn't be a read active already, but let's check just to be
	 * sure.
	 */
	if (PqCommReadingMsg)
		ereport(FATAL,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("terminating connection because protocol sync was lost")));

	PqCommReadingMsg = true;
}


/* --------------------------------
 *		pq_endmsgread	- finish reading message.
 *
 *		This must be called after reading a V2 protocol message with
 *		pq_getstring() and friends, to indicate that we have read the whole
 *		message. In V3 protocol, pq_getmessage() does this implicitly.
 * --------------------------------
 */
void
pq_endmsgread(void)
{
	Assert(PqCommReadingMsg);

	PqCommReadingMsg = false;
}

/* --------------------------------
 *		pq_is_reading_msg - are we currently reading a message?
 *
 * This is used in error recovery at the outer idle loop to detect if we have
 * lost protocol sync, and need to terminate the connection. pq_startmsgread()
 * will check for that too, but it's nicer to detect it earlier.
 * --------------------------------
 */
bool
pq_is_reading_msg(void)
{
	return PqCommReadingMsg;
}

/* --------------------------------
 *		pq_getmessage	- get a message with length word from connection
 *
 *		The return value is placed in an expansible StringInfo, which has
 *		already been initialized by the caller.
 *		Only the message body is placed in the StringInfo; the length word
 *		is removed.  Also, s->cursor is initialized to zero for convenience
 *		in scanning the message contents.
 *
 *		If maxlen is not zero, it is an upper limit on the length of the
 *		message we are willing to accept.  We abort the connection (by
 *		returning EOF) if client tries to send more than that.
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
int
pq_getmessage(StringInfo s, int maxlen)
{
	int32		len;

	Assert(PqCommReadingMsg);

	resetStringInfo(s);

	/* Read message length word */
	if (pq_getbytes((char *) &len, 4) == EOF)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("unexpected EOF within message length word")));
		return EOF;
	}

	len = ntohl(len);

	if (len < 4 ||
		(maxlen > 0 && len > maxlen))
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid message length")));
		return EOF;
	}

	len -= 4;					/* discount length itself */

	if (len > 0)
	{
		/*
		 * Allocate space for message.  If we run out of room (ridiculously
		 * large message), we will elog(ERROR), but we want to discard the
		 * message body so as not to lose communication sync.
		 */
		PG_TRY();
		{
			enlargeStringInfo(s, len);
		}
		PG_CATCH();
		{
			if (pq_discardbytes(len) == EOF)
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("incomplete message from client")));

			/* we discarded the rest of the message so we're back in sync. */
			PqCommReadingMsg = false;
			PG_RE_THROW();
		}
		PG_END_TRY();

		/* And grab the message */
		if (pq_getbytes(s->data, len) == EOF)
		{
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("incomplete message from client")));
			return EOF;
		}
		s->len = len;
		/* Place a trailing null per StringInfo convention */
		s->data[len] = '\0';
	}

	/* finished reading the message. */
	PqCommReadingMsg = false;

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
	int			res;

	/* Should only be called by old-style COPY OUT */
	Assert(DoingCopyOut);
	/* No-op if reentrant call */
	if (PqCommBusy)
		return 0;
	PqCommBusy = true;
	res = internal_putbytes(s, len);
	PqCommBusy = false;
	return res;
}

static int
internal_putbytes(const char *s, size_t len)
{
	size_t		amount;

	while (len > 0)
	{
		/* If buffer is full, then flush it out */
		if (PqSendPointer >= PqSendBufferSize)
		{
			pq_set_nonblocking(false);
			if (internal_flush())
				return EOF;
		}
		amount = PqSendBufferSize - PqSendPointer;
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
	int			res;

	/* No-op if reentrant call */
	if (PqCommBusy)
		return 0;
	PqCommBusy = true;
	pq_set_nonblocking(false);
	res = internal_flush();
	PqCommBusy = false;
	return res;
}

/* --------------------------------
 *		internal_flush - flush pending output
 *
 * Returns 0 if OK (meaning everything was sent, or operation would block
 * and the socket is in non-blocking mode), or EOF if trouble.
 * --------------------------------
 */
static int
internal_flush(void)
{
	static int	last_reported_send_errno = 0;

	char	   *bufptr = PqSendBuffer + PqSendStart;
	char	   *bufend = PqSendBuffer + PqSendPointer;

	while (bufptr < bufend)
	{
		int			r;

		r = secure_write(MyProcPort, bufptr, bufend - bufptr);

		if (r <= 0)
		{
			if (errno == EINTR)
				continue;		/* Ok if we were interrupted */

			/*
			 * Ok if no data writable without blocking, and the socket is in
			 * non-blocking mode.
			 */
			if (errno == EAGAIN ||
				errno == EWOULDBLOCK)
			{
				return 0;
			}

			/*
			 * Careful: an ereport() that tries to write to the client would
			 * cause recursion to here, leading to stack overflow and core
			 * dump!  This message must go *only* to the postmaster log.
			 *
			 * If a client disconnects while we're in the midst of output, we
			 * might write quite a bit of data before we get to a safe query
			 * abort point.  So, suppress duplicate log messages.
			 */
			if (errno != last_reported_send_errno)
			{
				last_reported_send_errno = errno;
				ereport(COMMERROR,
						(errcode_for_socket_access(),
						 errmsg("could not send data to client: %m")));
			}

			/*
			 * We drop the buffered data anyway so that processing can
			 * continue, even though we'll probably quit soon. We also set a
			 * flag that'll cause the next CHECK_FOR_INTERRUPTS to terminate
			 * the connection.
			 */
			PqSendStart = PqSendPointer = 0;
			ClientConnectionLost = 1;
			InterruptPending = 1;
			return EOF;
		}

		last_reported_send_errno = 0;	/* reset after any successful send */
		bufptr += r;
		PqSendStart += r;
	}

	PqSendStart = PqSendPointer = 0;
	return 0;
}

/* --------------------------------
 *		pq_flush_if_writable - flush pending output if writable without blocking
 *
 * Returns 0 if OK, or EOF if trouble.
 * --------------------------------
 */
int
pq_flush_if_writable(void)
{
	int			res;

	/* Quick exit if nothing to do */
	if (PqSendPointer == PqSendStart)
		return 0;

	/* No-op if reentrant call */
	if (PqCommBusy)
		return 0;

	/* Temporarily put the socket into non-blocking mode */
	pq_set_nonblocking(true);

	PqCommBusy = true;
	res = internal_flush();
	PqCommBusy = false;
	return res;
}

/* --------------------------------
 *		pq_is_send_pending	- is there any pending data in the output buffer?
 * --------------------------------
 */
bool
pq_is_send_pending(void)
{
	return (PqSendStart < PqSendPointer);
}

/* --------------------------------
 * Message-level I/O routines begin here.
 *
 * These routines understand about the old-style COPY OUT protocol.
 * --------------------------------
 */


/* --------------------------------
 *		pq_putmessage	- send a normal message (suppressed in COPY OUT mode)
 *
 *		If msgtype is not '\0', it is a message type code to place before
 *		the message body.  If msgtype is '\0', then the message has no type
 *		code (this is only valid in pre-3.0 protocols).
 *
 *		len is the length of the message body data at *s.  In protocol 3.0
 *		and later, a message length word (equal to len+4 because it counts
 *		itself too) is inserted by this routine.
 *
 *		All normal messages are suppressed while old-style COPY OUT is in
 *		progress.  (In practice only a few notice messages might get emitted
 *		then; dropping them is annoying, but at least they will still appear
 *		in the postmaster log.)
 *
 *		We also suppress messages generated while pqcomm.c is busy.  This
 *		avoids any possibility of messages being inserted within other
 *		messages.  The only known trouble case arises if SIGQUIT occurs
 *		during a pqcomm.c routine --- quickdie() will try to send a warning
 *		message, and the most reasonable approach seems to be to drop it.
 *
 *		returns 0 if OK, EOF if trouble
 * --------------------------------
 */
int
pq_putmessage(char msgtype, const char *s, size_t len)
{
	if (DoingCopyOut || PqCommBusy)
		return 0;
	PqCommBusy = true;
	if (msgtype)
		if (internal_putbytes(&msgtype, 1))
			goto fail;
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		uint32		n32;

		n32 = htonl((uint32) (len + 4));
		if (internal_putbytes((char *) &n32, 4))
			goto fail;
	}
	if (internal_putbytes(s, len))
		goto fail;
	PqCommBusy = false;
	return 0;

fail:
	PqCommBusy = false;
	return EOF;
}

/* --------------------------------
 *		pq_putmessage_noblock	- like pq_putmessage, but never blocks
 *
 *		If the output buffer is too small to hold the message, the buffer
 *		is enlarged.
 */
void
pq_putmessage_noblock(char msgtype, const char *s, size_t len)
{
	int res		PG_USED_FOR_ASSERTS_ONLY;
	int			required;

	/*
	 * Ensure we have enough space in the output buffer for the message header
	 * as well as the message itself.
	 */
	required = PqSendPointer + 1 + 4 + len;
	if (required > PqSendBufferSize)
	{
		PqSendBuffer = repalloc(PqSendBuffer, required);
		PqSendBufferSize = required;
	}
	res = pq_putmessage(msgtype, s, len);
	Assert(res == 0);			/* should not fail when the message fits in
								 * buffer */
}


/* --------------------------------
 *		pq_startcopyout - inform libpq that an old-style COPY OUT transfer
 *			is beginning
 * --------------------------------
 */
void
pq_startcopyout(void)
{
	DoingCopyOut = true;
}

/* --------------------------------
 *		pq_endcopyout	- end an old-style COPY OUT transfer
 *
 *		If errorAbort is indicated, we are aborting a COPY OUT due to an error,
 *		and must send a terminator line.  Since a partial data line might have
 *		been emitted, send a couple of newlines first (the first one could
 *		get absorbed by a backslash...)  Note that old-style COPY OUT does
 *		not allow binary transfers, so a textual terminator is always correct.
 * --------------------------------
 */
void
pq_endcopyout(bool errorAbort)
{
	if (!DoingCopyOut)
		return;
	if (errorAbort)
		pq_putbytes("\n\n\\.\n", 5);
	/* in non-error case, copy.c will have emitted the terminator line */
	DoingCopyOut = false;
}


/*
 * Support for TCP Keepalive parameters
 */

/*
 * On Windows, we need to set both idle and interval at the same time.
 * We also cannot reset them to the default (setting to zero will
 * actually set them to zero, not default), therefor we fallback to
 * the out-of-the-box default instead.
 */
#if defined(WIN32) && defined(SIO_KEEPALIVE_VALS)
static int
pq_setkeepaliveswin32(Port *port, int idle, int interval)
{
	struct tcp_keepalive ka;
	DWORD		retsize;

	if (idle <= 0)
		idle = 2 * 60 * 60;		/* default = 2 hours */
	if (interval <= 0)
		interval = 1;			/* default = 1 second */

	ka.onoff = 1;
	ka.keepalivetime = idle * 1000;
	ka.keepaliveinterval = interval * 1000;

	if (WSAIoctl(port->sock,
				 SIO_KEEPALIVE_VALS,
				 (LPVOID) &ka,
				 sizeof(ka),
				 NULL,
				 0,
				 &retsize,
				 NULL,
				 NULL)
		!= 0)
	{
		elog(LOG, "WSAIoctl(SIO_KEEPALIVE_VALS) failed: %ui",
			 WSAGetLastError());
		return STATUS_ERROR;
	}
	if (port->keepalives_idle != idle)
		port->keepalives_idle = idle;
	if (port->keepalives_interval != interval)
		port->keepalives_interval = interval;
	return STATUS_OK;
}
#endif

int
pq_getkeepalivesidle(Port *port)
{
#if defined(TCP_KEEPIDLE) || defined(TCP_KEEPALIVE) || defined(WIN32)
	if (port == NULL || IS_AF_UNIX(port->laddr.addr.ss_family))
		return 0;

	if (port->keepalives_idle != 0)
		return port->keepalives_idle;

	if (port->default_keepalives_idle == 0)
	{
#ifndef WIN32
		ACCEPT_TYPE_ARG3 size = sizeof(port->default_keepalives_idle);

#ifdef TCP_KEEPIDLE
		if (getsockopt(port->sock, IPPROTO_TCP, TCP_KEEPIDLE,
					   (char *) &port->default_keepalives_idle,
					   &size) < 0)
		{
			elog(LOG, "getsockopt(TCP_KEEPIDLE) failed: %m");
			port->default_keepalives_idle = -1; /* don't know */
		}
#else
		if (getsockopt(port->sock, IPPROTO_TCP, TCP_KEEPALIVE,
					   (char *) &port->default_keepalives_idle,
					   &size) < 0)
		{
			elog(LOG, "getsockopt(TCP_KEEPALIVE) failed: %m");
			port->default_keepalives_idle = -1; /* don't know */
		}
#endif   /* TCP_KEEPIDLE */
#else							/* WIN32 */
		/* We can't get the defaults on Windows, so return "don't know" */
		port->default_keepalives_idle = -1;
#endif   /* WIN32 */
	}

	return port->default_keepalives_idle;
#else
	return 0;
#endif
}

int
pq_setkeepalivesidle(int idle, Port *port)
{
	if (port == NULL || IS_AF_UNIX(port->laddr.addr.ss_family))
		return STATUS_OK;

#if defined(TCP_KEEPIDLE) || defined(TCP_KEEPALIVE) || defined(SIO_KEEPALIVE_VALS)
	if (idle == port->keepalives_idle)
		return STATUS_OK;

#ifndef WIN32
	if (port->default_keepalives_idle <= 0)
	{
		if (pq_getkeepalivesidle(port) < 0)
		{
			if (idle == 0)
				return STATUS_OK;		/* default is set but unknown */
			else
				return STATUS_ERROR;
		}
	}

	if (idle == 0)
		idle = port->default_keepalives_idle;

#ifdef TCP_KEEPIDLE
	if (setsockopt(port->sock, IPPROTO_TCP, TCP_KEEPIDLE,
				   (char *) &idle, sizeof(idle)) < 0)
	{
		elog(LOG, "setsockopt(TCP_KEEPIDLE) failed: %m");
		return STATUS_ERROR;
	}
#else
	if (setsockopt(port->sock, IPPROTO_TCP, TCP_KEEPALIVE,
				   (char *) &idle, sizeof(idle)) < 0)
	{
		elog(LOG, "setsockopt(TCP_KEEPALIVE) failed: %m");
		return STATUS_ERROR;
	}
#endif

	port->keepalives_idle = idle;
#else							/* WIN32 */
	return pq_setkeepaliveswin32(port, idle, port->keepalives_interval);
#endif
#else							/* TCP_KEEPIDLE || SIO_KEEPALIVE_VALS */
	if (idle != 0)
	{
		elog(LOG, "setting the keepalive idle time is not supported");
		return STATUS_ERROR;
	}
#endif
	return STATUS_OK;
}

int
pq_getkeepalivesinterval(Port *port)
{
#if defined(TCP_KEEPINTVL) || defined(SIO_KEEPALIVE_VALS)
	if (port == NULL || IS_AF_UNIX(port->laddr.addr.ss_family))
		return 0;

	if (port->keepalives_interval != 0)
		return port->keepalives_interval;

	if (port->default_keepalives_interval == 0)
	{
#ifndef WIN32
		ACCEPT_TYPE_ARG3 size = sizeof(port->default_keepalives_interval);

		if (getsockopt(port->sock, IPPROTO_TCP, TCP_KEEPINTVL,
					   (char *) &port->default_keepalives_interval,
					   &size) < 0)
		{
			elog(LOG, "getsockopt(TCP_KEEPINTVL) failed: %m");
			port->default_keepalives_interval = -1;		/* don't know */
		}
#else
		/* We can't get the defaults on Windows, so return "don't know" */
		port->default_keepalives_interval = -1;
#endif   /* WIN32 */
	}

	return port->default_keepalives_interval;
#else
	return 0;
#endif
}

int
pq_setkeepalivesinterval(int interval, Port *port)
{
	if (port == NULL || IS_AF_UNIX(port->laddr.addr.ss_family))
		return STATUS_OK;

#if defined(TCP_KEEPINTVL) || defined (SIO_KEEPALIVE_VALS)
	if (interval == port->keepalives_interval)
		return STATUS_OK;

#ifndef WIN32
	if (port->default_keepalives_interval <= 0)
	{
		if (pq_getkeepalivesinterval(port) < 0)
		{
			if (interval == 0)
				return STATUS_OK;		/* default is set but unknown */
			else
				return STATUS_ERROR;
		}
	}

	if (interval == 0)
		interval = port->default_keepalives_interval;

	if (setsockopt(port->sock, IPPROTO_TCP, TCP_KEEPINTVL,
				   (char *) &interval, sizeof(interval)) < 0)
	{
		elog(LOG, "setsockopt(TCP_KEEPINTVL) failed: %m");
		return STATUS_ERROR;
	}

	port->keepalives_interval = interval;
#else							/* WIN32 */
	return pq_setkeepaliveswin32(port, port->keepalives_idle, interval);
#endif
#else
	if (interval != 0)
	{
		elog(LOG, "setsockopt(TCP_KEEPINTVL) not supported");
		return STATUS_ERROR;
	}
#endif

	return STATUS_OK;
}

int
pq_getkeepalivescount(Port *port)
{
#ifdef TCP_KEEPCNT
	if (port == NULL || IS_AF_UNIX(port->laddr.addr.ss_family))
		return 0;

	if (port->keepalives_count != 0)
		return port->keepalives_count;

	if (port->default_keepalives_count == 0)
	{
		ACCEPT_TYPE_ARG3 size = sizeof(port->default_keepalives_count);

		if (getsockopt(port->sock, IPPROTO_TCP, TCP_KEEPCNT,
					   (char *) &port->default_keepalives_count,
					   &size) < 0)
		{
			elog(LOG, "getsockopt(TCP_KEEPCNT) failed: %m");
			port->default_keepalives_count = -1;		/* don't know */
		}
	}

	return port->default_keepalives_count;
#else
	return 0;
#endif
}

int
pq_setkeepalivescount(int count, Port *port)
{
	if (port == NULL || IS_AF_UNIX(port->laddr.addr.ss_family))
		return STATUS_OK;

#ifdef TCP_KEEPCNT
	if (count == port->keepalives_count)
		return STATUS_OK;

	if (port->default_keepalives_count <= 0)
	{
		if (pq_getkeepalivescount(port) < 0)
		{
			if (count == 0)
				return STATUS_OK;		/* default is set but unknown */
			else
				return STATUS_ERROR;
		}
	}

	if (count == 0)
		count = port->default_keepalives_count;

	if (setsockopt(port->sock, IPPROTO_TCP, TCP_KEEPCNT,
				   (char *) &count, sizeof(count)) < 0)
	{
		elog(LOG, "setsockopt(TCP_KEEPCNT) failed: %m");
		return STATUS_ERROR;
	}

	port->keepalives_count = count;
#else
	if (count != 0)
	{
		elog(LOG, "setsockopt(TCP_KEEPCNT) not supported");
		return STATUS_ERROR;
	}
#endif

	return STATUS_OK;
}
