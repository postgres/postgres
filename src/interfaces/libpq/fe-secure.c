/*-------------------------------------------------------------------------
 *
 * fe-connect.c
 *	  functions related to setting up a secure connection to the backend.
 *	  Secure connections are expected to provide confidentiality,
 *	  message integrity and endpoint authentication.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-secure.c,v 1.1 2002/06/14 04:23:17 momjian Exp $
 *	  
 * NOTES
 *	  The client *requires* a valid server certificate.  Since
 *	  SSH tunnels provide anonymous confidentiality, the presumption
 *	  is that sites that want endpoint authentication will use the
 *	  direct SSL support, while sites that are comfortable with
 *	  anonymous connections will use SSH tunnels.
 *
 *	  This code verifies the server certificate, to detect simple
 *	  "man-in-the-middle" and "impersonation" attacks.  The 
 *	  server certificate, or better yet the CA certificate used
 *	  to sign the server certificate, should be present in the
 *	  "$HOME/.postgresql/root.crt" file.  If this file isn't
 *	  readable, or the server certificate can't be validated, 
 *	  secure_open_client() will return an error code.
 *
 *	  Additionally, the server certificate's "common name" must
 *	  resolve to the other end of the socket.  This makes it
 *	  substantially harder to pull off a "man-in-the-middle" or
 *	  "impersonation" attack even if the server's private key
 *	  has been stolen.  This check limits acceptable network
 *	  layers to Unix sockets (weird, but legal), TCPv4 and TCPv6.
 *
 *	  Unfortunately neither the current front- or back-end handle
 *	  failure gracefully, resulting in the backend hiccupping.
 *	  This points out problems in each (the frontend shouldn't even
 *	  try to do SSL if secure_initialize() fails, and the backend
 *	  shouldn't crash/recover if an SSH negotiation fails.  The 
 *	  backend definitely needs to be fixed, to prevent a "denial
 *	  of service" attack, but I don't know enough about how the 
 *	  backend works (especially that pre-SSL negotiation) to identify
 *	  a fix.
 *
 * OS DEPENDENCIES
 *	  The code currently assumes a POSIX password entry.  How should
 *	  Windows and Mac users be handled?
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

#include "postgres_fe.h"

#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>

#include "libpq-fe.h"
#include "libpq-int.h"
#include "fe-auth.h"
#include "pqsignal.h"

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

#include <pwd.h>
#include <sys/stat.h>

#ifdef USE_SSL
#include <openssl/ssl.h>
#include <openssl/e_os.h>
#endif /* USE_SSL */

int secure_initialize(PGconn *);
void secure_destroy(void);
int secure_open_client(PGconn *);
void secure_close(PGconn *);
ssize_t secure_read(PGconn *, void *ptr, size_t len);
ssize_t secure_write(PGconn *, const void *ptr, size_t len);

#ifdef USE_SSL
static int verify_cb(int ok, X509_STORE_CTX *ctx);
static int verify_peer(PGconn *);
static int initialize_SSL(PGconn *);
static void destroy_SSL(void);
static int open_client_SSL(PGconn *);
static void close_SSL(PGconn *);
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
secure_initialize (PGconn *conn)
{
	int r = 0;

#ifdef USE_SSL
	r = initialize_SSL(conn);
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
secure_open_client (PGconn *conn)
{
	int r = 0;

#ifdef USE_SSL
	r = open_client_SSL(conn);
#endif

	return r;
}

/*
 *	Close secure session.
 */
void
secure_close (PGconn *conn)
{
#ifdef USE_SSL
	if (conn->ssl)
		close_SSL(conn);
#endif
}

/*
 *	Read data from a secure connection.
 */
ssize_t
secure_read (PGconn *conn, void *ptr, size_t len)
{
	ssize_t n;

#ifdef USE_SSL
	if (conn->ssl)
	{
		n = SSL_read(conn->ssl, ptr, len);
		switch (SSL_get_error(conn->ssl, n))
		{
		case SSL_ERROR_NONE:
			break;
		case SSL_ERROR_WANT_READ:
			break;
		case SSL_ERROR_SYSCALL:
			SOCK_ERRNO = get_last_socket_error();
			printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("SSL SYSCALL error: %s\n"), 
				SOCK_STRERROR(SOCK_ERRNO));
			break;
		case SSL_ERROR_SSL:
			printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("SSL error: %s\n"), SSLerrmessage());
			/* fall through */
		case SSL_ERROR_ZERO_RETURN:
			secure_close(conn);
			SOCK_ERRNO = ECONNRESET;
			n = -1;
			break;
		}
	}
	else
#endif
	n = recv(conn->sock, ptr, len, 0);

	return n;
}

/*
 *	Write data to a secure connection.
 */
ssize_t
secure_write (PGconn *conn, const void *ptr, size_t len)
{
	ssize_t n;

#ifndef WIN32
	pqsigfunc oldsighandler = pqsignal(SIGPIPE, SIG_IGN);
#endif

#ifdef USE_SSL
	if (conn->ssl)
	{
		n = SSL_write(conn->ssl, ptr, len);
		switch (SSL_get_error(conn->ssl, n))
		{
		case SSL_ERROR_NONE:
			break;
		case SSL_ERROR_WANT_WRITE:
			break;
		case SSL_ERROR_SYSCALL:
			SOCK_ERRNO = get_last_socket_error();
			printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("SSL SYSCALL error: %s\n"),
				SOCK_STRERROR(SOCK_ERRNO));
			break;
		case SSL_ERROR_SSL:
			printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("SSL error: %s\n"), SSLerrmessage());
			/* fall through */
		case SSL_ERROR_ZERO_RETURN:
			secure_close(conn);
			SOCK_ERRNO = ECONNRESET;
			n = -1;
			break;
		}
	}
	else
#endif
	n = send(conn->sock, ptr, len, 0);

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
 *	Certificate verification callback
 *
 *	This callback allows us to log intermediate problems during
 *	verification, but there doesn't seem to be a clean way to get
 *	our PGconn * structure.  So we can't log anything!
 *
 *	This callback also allows us to override the default acceptance
 *	criteria (e.g., accepting self-signed or expired certs), but
 *	for now we accept the default checks.
 */
static int
verify_cb (int ok, X509_STORE_CTX *ctx)
{
	return ok;
}

/*
 *	Verify that common name resolves to peer.
 *	This function is not thread-safe due to gethostbyname2().
 */
static int
verify_peer (PGconn *conn)
{
	struct hostent *h = NULL;
	struct sockaddr addr;
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;
	socklen_t len;
	char **s;
	unsigned long l;

	/* get the address on the other side of the socket */
	len = sizeof(addr);
	if (getpeername(conn->sock, &addr, &len) == -1)
	{
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("error querying socket: %s\n"), 
			SOCK_STRERROR(SOCK_ERRNO));
		return -1;
	}

	/* weird, but legal case */
	if (addr.sa_family == AF_UNIX)
		return 0;

	/* what do we know about the peer's common name? */
	if ((h = gethostbyname2(conn->peer_cn, addr.sa_family)) == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("error getting information about host (%s): %s\n"),
			conn->peer_cn, hstrerror(h_errno));
		return -1;
	}

	/* does the address match? */
	switch (addr.sa_family)
	{
	case AF_INET:
		sin = (struct sockaddr_in *) &addr;
		for (s = h->h_addr_list; *s != NULL; s++)
		{
			if (!memcmp(&sin->sin_addr.s_addr, *s, h->h_length))
				return 0;
		}
		break;

	case AF_INET6:
		sin6 = (struct sockaddr_in6 *) &addr;
		for (s = h->h_addr_list; *s != NULL; s++)
		{
			if (!memcmp(sin6->sin6_addr.in6_u.u6_addr8, *s, h->h_length))
				return 0;
		}
		break;

	default:
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("sorry, this protocol not yet supported\n"));
		return -1;
	}

	/* the prior test should be definitive, but in practice
	 * it sometimes fails.  So we also check the aliases.  */
	for (s = h->h_aliases; *s != NULL; s++)
	{
		if (strcasecmp(conn->peer_cn, *s) == 0)
			return 0;
	}

	/* generate protocol-aware error message */
	switch (addr.sa_family)
	{
	case AF_INET:
		sin = (struct sockaddr_in *) &addr;
		l = ntohl(sin->sin_addr.s_addr);
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext(
				"server common name '%s' does not resolve to %ld.%ld.%ld.%ld\n"),
			conn->peer_cn, (l >> 24) % 0x100, (l >> 16) % 0x100,
			(l >> 8) % 0x100, l % 0x100);
		break;
	default:
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext(
				"server common name '%s' does not resolve to peer address\n"),
			conn->peer_cn);
	}

	return -1;
}

/*
 *	Initialize global SSL context.
 */
static int
initialize_SSL (PGconn *conn)
{
	struct stat buf;
	struct passwd *pwd;
	char fnbuf[2048];

	if (!SSL_context)
	{
		SSL_library_init();
		SSL_load_error_strings();
		SSL_context = SSL_CTX_new(TLSv1_method());
		if (!SSL_context)
		{
			printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("could not create SSL context: %s\n"),
							  SSLerrmessage());
			return -1;
		}
	}

	if ((pwd = getpwuid(getuid())) != NULL)
	{
		snprintf(fnbuf, sizeof fnbuf, "%s/.postgresql/root.crt",
			pwd->pw_dir);
		if (stat(fnbuf, &buf) == -1)
		{
			printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("could not read  root cert list(%s): %s"),
				fnbuf, strerror(errno));
			return -1;
		}
		if (!SSL_CTX_load_verify_locations(SSL_context, fnbuf, 0))
		{
			printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("could not read root cert list (%s): %s"),
				fnbuf, SSLerrmessage());
			return -1;
		}
	}

	SSL_CTX_set_verify(SSL_context, 
		SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify_cb);
	SSL_CTX_set_verify_depth(SSL_context, 1);

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
open_client_SSL (PGconn *conn)
{
	int r;

	if (!(conn->ssl = SSL_new(SSL_context)) ||
		!SSL_set_fd(conn->ssl, conn->sock) ||
		SSL_connect(conn->ssl) <= 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("could not establish SSL connection: %s\n"),
						  SSLerrmessage());
		close_SSL(conn);
		return -1;
	}

	/* check the certificate chain of the server */
	/* this eliminates simple man-in-the-middle attacks and
	 * simple impersonations */
	r = SSL_get_verify_result(conn->ssl);
	if (r != X509_V_OK)
	{
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("certificate could not be validated: %s\n"),
			X509_verify_cert_error_string(r));
		close_SSL(conn);
		return -1;
	}

	/* pull out server distinguished and common names */
	conn->peer = SSL_get_peer_certificate(conn->ssl);
	if (conn->peer == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("certificate could not be obtained: %s\n"),
			SSLerrmessage());
		close_SSL(conn);
		return -1;
	}

	X509_NAME_oneline(X509_get_subject_name(conn->peer),
		conn->peer_dn, sizeof(conn->peer_dn));
	conn->peer_dn[sizeof(conn->peer_dn)-1] = '\0';

	X509_NAME_get_text_by_NID(X509_get_subject_name(conn->peer),
		NID_commonName, conn->peer_cn, SM_USER);
	conn->peer_cn[SM_USER] = '\0';

	/* verify that the common name resolves to peer */
	/* this is necessary to eliminate man-in-the-middle attacks
	 * and impersonations where the attacker somehow learned
	 * the server's private key */
	if (verify_peer(conn) == -1)
	{
		close_SSL(conn);
		return -1;
	}

	return 0;
}

/*
 *	Close SSL connection.
 */
static void
close_SSL (PGconn *conn)
{
	if (conn->ssl)
	{
		SSL_shutdown(conn->ssl);
		SSL_free(conn->ssl);
		conn->ssl = NULL;
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

/*
 *	Return pointer to SSL object.
 */
SSL *
PQgetssl(PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->ssl;
}
#endif /* USE_SSL */
