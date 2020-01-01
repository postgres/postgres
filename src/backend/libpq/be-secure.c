/*-------------------------------------------------------------------------
 *
 * be-secure.c
 *	  functions related to setting up a secure connection to the frontend.
 *	  Secure connections are expected to provide confidentiality,
 *	  message integrity and endpoint authentication.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/be-secure.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

#include "libpq/libpq.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"

char	   *ssl_library;
char	   *ssl_cert_file;
char	   *ssl_key_file;
char	   *ssl_ca_file;
char	   *ssl_crl_file;
char	   *ssl_dh_params_file;
char	   *ssl_passphrase_command;
bool		ssl_passphrase_command_supports_reload;

#ifdef USE_SSL
bool		ssl_loaded_verify_locations = false;
#endif

/* GUC variable controlling SSL cipher list */
char	   *SSLCipherSuites = NULL;

/* GUC variable for default ECHD curve. */
char	   *SSLECDHCurve;

/* GUC variable: if false, prefer client ciphers */
bool		SSLPreferServerCiphers;

int			ssl_min_protocol_version;
int			ssl_max_protocol_version;

/* ------------------------------------------------------------ */
/*			 Procedures common to all secure sessions			*/
/* ------------------------------------------------------------ */

/*
 *	Initialize global context.
 *
 * If isServerStart is true, report any errors as FATAL (so we don't return).
 * Otherwise, log errors at LOG level and return -1 to indicate trouble,
 * preserving the old SSL state if any.  Returns 0 if OK.
 */
int
secure_initialize(bool isServerStart)
{
#ifdef USE_SSL
	return be_tls_init(isServerStart);
#else
	return 0;
#endif
}

/*
 *	Destroy global context, if any.
 */
void
secure_destroy(void)
{
#ifdef USE_SSL
	be_tls_destroy();
#endif
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

	ereport(DEBUG2,
			(errmsg("SSL connection from \"%s\"",
					port->peer_cn ? port->peer_cn : "(anonymous)")));
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
	int			waitfor;

	/* Deal with any already-pending interrupt condition. */
	ProcessClientReadInterrupt(false);

retry:
#ifdef USE_SSL
	waitfor = 0;
	if (port->ssl_in_use)
	{
		n = be_tls_read(port, ptr, len, &waitfor);
	}
	else
#endif
#ifdef ENABLE_GSS
	if (port->gss->enc)
	{
		n = be_gssapi_read(port, ptr, len);
		waitfor = WL_SOCKET_READABLE;
	}
	else
#endif
	{
		n = secure_raw_read(port, ptr, len);
		waitfor = WL_SOCKET_READABLE;
	}

	/* In blocking mode, wait until the socket is ready */
	if (n < 0 && !port->noblock && (errno == EWOULDBLOCK || errno == EAGAIN))
	{
		WaitEvent	event;

		Assert(waitfor);

		ModifyWaitEvent(FeBeWaitSet, 0, waitfor, NULL);

		WaitEventSetWait(FeBeWaitSet, -1 /* no timeout */ , &event, 1,
						 WAIT_EVENT_CLIENT_READ);

		/*
		 * If the postmaster has died, it's not safe to continue running,
		 * because it is the postmaster's job to kill us if some other backend
		 * exits uncleanly.  Moreover, we won't run very well in this state;
		 * helper processes like walwriter and the bgwriter will exit, so
		 * performance may be poor.  Finally, if we don't exit, pg_ctl will be
		 * unable to restart the postmaster without manual intervention, so no
		 * new connections can be accepted.  Exiting clears the deck for a
		 * postmaster restart.
		 *
		 * (Note that we only make this check when we would otherwise sleep on
		 * our latch.  We might still continue running for a while if the
		 * postmaster is killed in mid-query, or even through multiple queries
		 * if we never have to wait for read.  We don't want to burn too many
		 * cycles checking for this very rare condition, and this should cause
		 * us to exit quickly in most cases.)
		 */
		if (event.events & WL_POSTMASTER_DEATH)
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection due to unexpected postmaster exit")));

		/* Handle interrupt. */
		if (event.events & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			ProcessClientReadInterrupt(true);

			/*
			 * We'll retry the read. Most likely it will return immediately
			 * because there's still no data available, and we'll wait for the
			 * socket to become ready again.
			 */
		}
		goto retry;
	}

	/*
	 * Process interrupts that happened during a successful (or non-blocking,
	 * or hard-failed) read.
	 */
	ProcessClientReadInterrupt(false);

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
#ifdef WIN32
	pgwin32_noblock = true;
#endif
	n = recv(port->sock, ptr, len, 0);
#ifdef WIN32
	pgwin32_noblock = false;
#endif

	return n;
}


/*
 *	Write data to a secure connection.
 */
ssize_t
secure_write(Port *port, void *ptr, size_t len)
{
	ssize_t		n;
	int			waitfor;

	/* Deal with any already-pending interrupt condition. */
	ProcessClientWriteInterrupt(false);

retry:
	waitfor = 0;
#ifdef USE_SSL
	if (port->ssl_in_use)
	{
		n = be_tls_write(port, ptr, len, &waitfor);
	}
	else
#endif
#ifdef ENABLE_GSS
	if (port->gss->enc)
	{
		n = be_gssapi_write(port, ptr, len);
		waitfor = WL_SOCKET_WRITEABLE;
	}
	else
#endif
	{
		n = secure_raw_write(port, ptr, len);
		waitfor = WL_SOCKET_WRITEABLE;
	}

	if (n < 0 && !port->noblock && (errno == EWOULDBLOCK || errno == EAGAIN))
	{
		WaitEvent	event;

		Assert(waitfor);

		ModifyWaitEvent(FeBeWaitSet, 0, waitfor, NULL);

		WaitEventSetWait(FeBeWaitSet, -1 /* no timeout */ , &event, 1,
						 WAIT_EVENT_CLIENT_WRITE);

		/* See comments in secure_read. */
		if (event.events & WL_POSTMASTER_DEATH)
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection due to unexpected postmaster exit")));

		/* Handle interrupt. */
		if (event.events & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			ProcessClientWriteInterrupt(true);

			/*
			 * We'll retry the write. Most likely it will return immediately
			 * because there's still no buffer space available, and we'll wait
			 * for the socket to become ready again.
			 */
		}
		goto retry;
	}

	/*
	 * Process interrupts that happened during a successful (or non-blocking,
	 * or hard-failed) write.
	 */
	ProcessClientWriteInterrupt(false);

	return n;
}

ssize_t
secure_raw_write(Port *port, const void *ptr, size_t len)
{
	ssize_t		n;

#ifdef WIN32
	pgwin32_noblock = true;
#endif
	n = send(port->sock, ptr, len, 0);
#ifdef WIN32
	pgwin32_noblock = false;
#endif

	return n;
}
