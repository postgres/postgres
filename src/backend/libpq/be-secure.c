/*-------------------------------------------------------------------------
 *
 * be-connect.c
 *	  functions related to setting up a secure connection to the frontend.
 *	  Secure connections are expected to provide confidentiality,
 *	  message integrity and endpoint authentication.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/be-secure.c,v 1.1 2002/06/14 04:23:17 momjian Exp $
 *	  
 * PATCH LEVEL
 *	  milestone 1: fix basic coding errors
 *	  [*] existing SSL code pulled out of existing files.
 *	  [*] SSL_get_error() after SSL_read() and SSL_write(),
 *	      SSL_shutdown(), default to TLSv1.
 *	
 *	  milestone 2: provide endpoint authentication (server)
 *	  [*] client verifies server cert
 *	  [*] client verifies server hostname
 *
 *	  milestone 3: improve confidentially, support perfect forward secrecy
 *	  [ ] use 'random' file, read from '/dev/urandom?'
 *	  [ ] emphermal DH keys, default values
 *	  [ ] periodic renegotiation
 *
 *	  milestone 4: provide endpoint authentication (client)
 *	  [ ] server verifies client certificates
 *
 *	  milestone 5: provide informational callbacks
 *	  [ ] provide informational callbacks
 *
 *	  other changes
 *	  [ ] tcp-wrappers
 *	  [ ] more informative psql
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#include "libpq/libpq.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"

#ifdef WIN32
#include "win32.h"
#else
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#endif


#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

#ifdef USE_SSL
#include <openssl/ssl.h>
#include <openssl/e_os.h>
#endif

extern void ExitPostmaster(int);
extern void postmaster_error(const char *fmt,...);

int secure_initialize(void);
void secure_destroy(void);
int secure_open_server(Port *);
void secure_close(Port *);
ssize_t secure_read(Port *, void *ptr, size_t len);
ssize_t secure_write(Port *, const void *ptr, size_t len);

#ifdef USE_SSL
static int initialize_SSL(void);
static void destroy_SSL(void);
static int open_server_SSL(Port *);
static void close_SSL(Port *);
static const char *SSLerrmessage(void);
#endif

#ifdef USE_SSL
static SSL_CTX *SSL_context = NULL;
#endif

/* ------------------------------------------------------------ */
/*           Procedures common to all secure sessions           */
/* ------------------------------------------------------------ */

/*
 *	Initialize global context
 */
int
secure_initialize (void)
{
	int r = 0;

#ifdef USE_SSL
	r = initialize_SSL();
#endif

	return r;
}

/*
 *	Destroy global context
 */
void
secure_destroy (void)
{
#ifdef USE_SSL
	destroy_SSL();
#endif
}

/*
 *	Attempt to negotiate secure session.
 */
int 
secure_open_server (Port *port)
{
	int r = 0;

#ifdef USE_SSL
	r = open_server_SSL(port);
#endif

	return r;
}

/*
 *	Close secure session.
 */
void
secure_close (Port *port)
{
#ifdef USE_SSL
	if (port->ssl)
		close_SSL(port);
#endif
}

/*
 *	Read data from a secure connection.
 */
ssize_t
secure_read (Port *port, void *ptr, size_t len)
{
	ssize_t n;

#ifdef USE_SSL
	if (port->ssl)
	{
		n = SSL_read(port->ssl, ptr, len);
		switch (SSL_get_error(port->ssl, n))
		{
		case SSL_ERROR_NONE:
			break;
		case SSL_ERROR_WANT_READ:
			break;
		case SSL_ERROR_SYSCALL:
			errno = get_last_socket_error();
			elog(ERROR, "SSL SYSCALL error: %s", strerror(errno));
			break;
		case SSL_ERROR_SSL:
			elog(ERROR, "SSL error: %s", SSLerrmessage());
			/* fall through */
		case SSL_ERROR_ZERO_RETURN:
			secure_close(port);
			errno = ECONNRESET;
			n = -1;
			break;
		}
	}
	else
#endif
	n = recv(port->sock, ptr, len, 0);

	return n;
}

/*
 *	Write data to a secure connection.
 */
ssize_t
secure_write (Port *port, const void *ptr, size_t len)
{
	ssize_t n;

#ifndef WIN32
	pqsigfunc oldsighandler = pqsignal(SIGPIPE, SIG_IGN);
#endif

#ifdef USE_SSL
	if (port->ssl)
	{
		n = SSL_write(port->ssl, ptr, len);
		switch (SSL_get_error(port->ssl, n))
		{
		case SSL_ERROR_NONE:
			break;
		case SSL_ERROR_WANT_WRITE:
			break;
		case SSL_ERROR_SYSCALL:
			errno = get_last_socket_error();
			elog(ERROR, "SSL SYSCALL error: %s", strerror(errno));
			break;
		case SSL_ERROR_SSL:
			elog(ERROR, "SSL error: %s", SSLerrmessage());
			/* fall through */
		case SSL_ERROR_ZERO_RETURN:
			secure_close(port);
			errno = ECONNRESET;
			n = -1;
			break;
		}
	}
	else
#endif
	n = send(port->sock, ptr, len, 0);

#ifndef WIN32
	pqsignal(SIGPIPE, oldsighandler);
#endif

	return n;
}

/* ------------------------------------------------------------ */
/*                        SSL specific code                     */
/* ------------------------------------------------------------ */
#ifdef USE_SSL
/*
 *	Initialize global SSL context.
 */
static int
initialize_SSL (void)
{
	char fnbuf[2048];

	if (!SSL_context)
	{
		SSL_library_init();
		SSL_load_error_strings();
		SSL_context = SSL_CTX_new(TLSv1_method());
		if (!SSL_context)
		{
			postmaster_error("failed to create SSL context: %s",
							 SSLerrmessage());
			ExitPostmaster(1);
		}

		/*
		 *	Load and verify certificate and private key
		 */
		snprintf(fnbuf, sizeof(fnbuf), "%s/server.crt", DataDir);
		if (!SSL_CTX_use_certificate_file(SSL_context, fnbuf, SSL_FILETYPE_PEM))
		{
			postmaster_error("failed to load server certificate (%s): %s",
							 fnbuf, SSLerrmessage());
			ExitPostmaster(1);
		}
		snprintf(fnbuf, sizeof(fnbuf), "%s/server.key", DataDir);
		if (!SSL_CTX_use_PrivateKey_file(SSL_context, fnbuf, SSL_FILETYPE_PEM))
		{
			postmaster_error("failed to load private key file (%s): %s",
							 fnbuf, SSLerrmessage());
			ExitPostmaster(1);
		}
		if (!SSL_CTX_check_private_key(SSL_context))
		{
			postmaster_error("check of private key failed: %s",
							 SSLerrmessage());
			ExitPostmaster(1);
		}
	}

	return 0;
}

/*
 *	Destroy global SSL context.
 */
static void
destroy_SSL (void)
{
	if (SSL_context)
	{
		SSL_CTX_free(SSL_context);
		SSL_context = NULL;
	}
}

/*
 *	Attempt to negotiate SSL connection.
 */
static int
open_server_SSL (Port *port)
{
	if (!(port->ssl = SSL_new(SSL_context)) ||
		!SSL_set_fd(port->ssl, port->sock) ||
		SSL_accept(port->ssl) <= 0)
	{
		elog(ERROR, "failed to initialize SSL connection: %s", SSLerrmessage());
		close_SSL(port);
		return -1;
	}

	return 0;
}

/*
 *	Close SSL connection.
 */
static void
close_SSL (Port *port)
{
	if (port->ssl)
	{
		SSL_shutdown(port->ssl);
		SSL_free(port->ssl);
		port->ssl = NULL;
	}
}

/*
 * Obtain reason string for last SSL error
 *
 * Some caution is needed here since ERR_reason_error_string will
 * return NULL if it doesn't recognize the error code.  We don't
 * want to return NULL ever.
 */
static const char *
SSLerrmessage(void)
{
	unsigned long	errcode;
	const char	   *errreason;
	static char		errbuf[32];

	errcode = ERR_get_error();
	if (errcode == 0)
		return "No SSL error reported";
	errreason = ERR_reason_error_string(errcode);
	if (errreason != NULL)
		return errreason;
	snprintf(errbuf, sizeof(errbuf), "SSL error code %lu", errcode);
	return errbuf;
}

#endif /* USE_SSL */
