/*-------------------------------------------------------------------------
 *
 * be-secure.c
 *	  functions related to setting up a secure connection to the frontend.
 *	  Secure connections are expected to provide confidentiality,
 *	  message integrity and endpoint authentication.
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/be-secure.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

#include "libpq/libpq.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "storage/proc.h"


char	   *ssl_cert_file;
char	   *ssl_key_file;
char	   *ssl_ca_file;
char	   *ssl_crl_file;

/*
 *	How much data can be sent across a secure connection
 *	(total in both directions) before we require renegotiation.
 *	Set to 0 to disable renegotiation completely.
 */
int			ssl_renegotiation_limit;

#ifdef USE_SSL
bool ssl_loaded_verify_locations = false;
#endif

/* GUC variable controlling SSL cipher list */
char	   *SSLCipherSuites = NULL;

/* GUC variable for default ECHD curve. */
char	   *SSLECDHCurve;

/* GUC variable: if false, prefer client ciphers */
bool		SSLPreferServerCiphers;

/* ------------------------------------------------------------ */
/*			 Procedures common to all secure sessions			*/
/* ------------------------------------------------------------ */

/*
 *	Initialize global context
 */
int
secure_initialize(void)
{
#ifdef USE_SSL
	be_tls_init();
#endif

	return 0;
}

/*
 * Indicate if we have loaded the root CA store to verify certificates
 */
bool
secure_loaded_verify_locations(void)
{
#ifdef USE_SSL
	return ssl_loaded_verify_locations;
#else
	return false;
#endif
}

/*
 *	Attempt to negotiate secure session.
 */
int
secure_open_server(Port *port)
{
	int			r = 0;

#ifdef USE_SSL
	r = be_tls_open_server(port);
#endif

	return r;
}

/*
 *	Close secure session.
 */
void
secure_close(Port *port)
{
#ifdef USE_SSL
	if (port->ssl_in_use)
		be_tls_close(port);
#endif
}

/*
 *	Read data from a secure connection.
 */
ssize_t
secure_read(Port *port, void *ptr, size_t len)
{
	ssize_t		n;

retry:
#ifdef USE_SSL
	if (port->ssl_in_use)
	{
		n = be_tls_read(port, ptr, len);
	}
	else
#endif
	{
		n = secure_raw_read(port, ptr, len);
	}

	/* Process interrupts that happened while (or before) receiving. */
	ProcessClientReadInterrupt(); /* preserves errno */

	/* retry after processing interrupts */
	if (n < 0 && errno == EINTR)
	{
		goto retry;
	}
	return n;
}

ssize_t
secure_raw_read(Port *port, void *ptr, size_t len)
{
	ssize_t		n;

	/*
	 * Try to read from the socket without blocking. If it succeeds we're
	 * done, otherwise we'll wait for the socket using the latch mechanism.
	 */
rloop:
#ifdef WIN32
	pgwin32_noblock = true;
#endif
	n = recv(port->sock, ptr, len, 0);
#ifdef WIN32
	pgwin32_noblock = false;
#endif

	if (n < 0 && !port->noblock && (errno == EWOULDBLOCK || errno == EAGAIN))
	{
		int		w;
		int		save_errno = errno;

		w = WaitLatchOrSocket(MyLatch,
							  WL_LATCH_SET | WL_SOCKET_READABLE,
							  port->sock, 0);

		if (w & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			/*
			 * Force a return, so interrupts can be processed when not
			 * (possibly) underneath a ssl library.
			 */
			errno = EINTR;
			return -1;
		}
		else if (w & WL_SOCKET_READABLE)
		{
			goto rloop;
		}

		/*
		 * Restore errno, clobbered by WaitLatchOrSocket, so the caller can
		 * react properly.
		 */
		errno = save_errno;
	}

	return n;
}


/*
 *	Write data to a secure connection.
 */
ssize_t
secure_write(Port *port, void *ptr, size_t len)
{
	ssize_t		n;

retry:
#ifdef USE_SSL
	if (port->ssl_in_use)
	{
		n = be_tls_write(port, ptr, len);
	}
	else
#endif
	{
		n = secure_raw_write(port, ptr, len);
	}

	/*
	 * XXX: We'll, at some later point, likely want to add interrupt
	 * processing here.
	 */

	/*
	 * Retry after processing interrupts. This can be triggered even though we
	 * don't check for latch set's during writing yet, because SSL
	 * renegotiations might have required reading from the socket.
	 */
	if (n < 0 && errno == EINTR)
	{
		goto retry;
	}

	return n;
}

ssize_t
secure_raw_write(Port *port, const void *ptr, size_t len)
{
	ssize_t		n;

wloop:

#ifdef WIN32
	pgwin32_noblock = true;
#endif
	n = send(port->sock, ptr, len, 0);
#ifdef WIN32
	pgwin32_noblock = false;
#endif

	if (n < 0 && !port->noblock && (errno == EWOULDBLOCK || errno == EAGAIN))
	{
		int		w;
		int		save_errno = errno;

		/*
		 * We probably want to check for latches being set at some point
		 * here. That'd allow us to handle interrupts while blocked on
		 * writes. If set we'd not retry directly, but return. That way we
		 * don't do anything while (possibly) inside a ssl library.
		 */
		w = WaitLatchOrSocket(MyLatch,
							  WL_SOCKET_WRITEABLE,
							  port->sock, 0);

		if (w & WL_SOCKET_WRITEABLE)
		{
			goto wloop;
		}

		/*
		 * Restore errno, clobbered by WaitLatchOrSocket, so the caller can
		 * react properly.
		 */
		errno = save_errno;
	}

	return n;
}
