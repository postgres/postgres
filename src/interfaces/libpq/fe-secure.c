/*-------------------------------------------------------------------------
 *
 * fe-secure.c
 *	  functions related to setting up a secure connection to the backend.
 *	  Secure connections are expected to provide confidentiality,
 *	  message integrity and endpoint authentication.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-secure.c,v 1.32.2.1 2003/12/18 22:49:34 tgl Exp $
 *
 * NOTES
 *	  The client *requires* a valid server certificate.  Since
 *	  SSH tunnels provide anonymous confidentiality, the presumption
 *	  is that sites that want endpoint authentication will use the
 *	  direct SSL support, while sites that are comfortable with
 *	  anonymous connections will use SSH tunnels.
 *
 *	  This code verifies the server certificate, to detect simple
 *	  "man-in-the-middle" and "impersonation" attacks.	The
 *	  server certificate, or better yet the CA certificate used
 *	  to sign the server certificate, should be present in the
 *	  "$HOME/.postgresql/root.crt" file.  If this file isn't
 *	  readable, or the server certificate can't be validated,
 *	  pqsecure_open_client() will return an error code.
 *
 *	  Additionally, the server certificate's "common name" must
 *	  resolve to the other end of the socket.  This makes it
 *	  substantially harder to pull off a "man-in-the-middle" or
 *	  "impersonation" attack even if the server's private key
 *	  has been stolen.	This check limits acceptable network
 *	  layers to Unix sockets (weird, but legal), TCPv4 and TCPv6.
 *
 *	  Unfortunately neither the current front- or back-end handle
 *	  failure gracefully, resulting in the backend hiccupping.
 *	  This points out problems in each (the frontend shouldn't even
 *	  try to do SSL if pqsecure_initialize() fails, and the backend
 *	  shouldn't crash/recover if an SSH negotiation fails.  The
 *	  backend definitely needs to be fixed, to prevent a "denial
 *	  of service" attack, but I don't know enough about how the
 *	  backend works (especially that pre-SSL negotiation) to identify
 *	  a fix.
 *
 *	  ...
 *
 *	  Unlike the server's static private key, the client's
 *	  static private key ($HOME/.postgresql/postgresql.key)
 *	  should normally be stored encrypted.	However we still
 *	  support EPH since it's useful for other reasons.
 *
 *	  ...
 *
 *	  Client certificates are supported, if the server requests
 *	  or requires them.  Client certificates can be used for
 *	  authentication, to prevent sessions from being hijacked,
 *	  or to allow "road warriors" to access the database while
 *	  keeping it closed to everyone else.
 *
 *	  The user's certificate and private key are located in
 *		$HOME/.postgresql/postgresql.crt
 *	  and
 *		$HOME/.postgresql/postgresql.key
 *	  respectively.
 *
 *	  ...
 *
 *	  We don't provide informational callbacks here (like
 *	  info_cb() in be-secure.c), since there's mechanism to
 *	  display that information to the client.
 *
 * OS DEPENDENCIES
 *	  The code currently assumes a POSIX password entry.  How should
 *	  Windows and Mac users be handled?
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

#ifndef WIN32
#include <pwd.h>
#endif
#include <sys/stat.h>

#ifdef USE_SSL
#include <openssl/ssl.h>
#include <openssl/dh.h>
#endif   /* USE_SSL */


#ifdef USE_SSL
static int	verify_cb(int ok, X509_STORE_CTX *ctx);

#ifdef NOT_USED
static int	verify_peer(PGconn *);
#endif
static DH  *load_dh_file(int keylength);
static DH  *load_dh_buffer(const char *, size_t);
static DH  *tmp_dh_cb(SSL *s, int is_export, int keylength);
static int	client_cert_cb(SSL *, X509 **, EVP_PKEY **);
static int	initialize_SSL(PGconn *);
static void destroy_SSL(void);
static PostgresPollingStatusType open_client_SSL(PGconn *);
static void close_SSL(PGconn *);
static const char *SSLerrmessage(void);
#endif

#ifdef USE_SSL
static SSL_CTX *SSL_context = NULL;
#endif

/* ------------------------------------------------------------ */
/*						 Hardcoded values						*/
/* ------------------------------------------------------------ */

/*
 *	Hardcoded DH parameters, used in empheral DH keying.
 *	As discussed above, EDH protects the confidentiality of
 *	sessions even if the static private key is compromised,
 *	so we are *highly* motivated to ensure that we can use
 *	EDH even if the user... or an attacker... deletes the
 *	$HOME/.postgresql/dh*.pem files.
 *
 *	It's not critical that users have EPH keys, but it doesn't
 *	hurt and if it's missing someone will demand it, so....
 */
#ifdef USE_SSL

static const char file_dh512[] =
"-----BEGIN DH PARAMETERS-----\n\
MEYCQQD1Kv884bEpQBgRjXyEpwpy1obEAxnIByl6ypUM2Zafq9AKUJsCRtMIPWak\n\
XUGfnHy9iUsiGSa6q6Jew1XpKgVfAgEC\n\
-----END DH PARAMETERS-----\n";

static const char file_dh1024[] =
"-----BEGIN DH PARAMETERS-----\n\
MIGHAoGBAPSI/VhOSdvNILSd5JEHNmszbDgNRR0PfIizHHxbLY7288kjwEPwpVsY\n\
jY67VYy4XTjTNP18F1dDox0YbN4zISy1Kv884bEpQBgRjXyEpwpy1obEAxnIByl6\n\
ypUM2Zafq9AKUJsCRtMIPWakXUGfnHy9iUsiGSa6q6Jew1XpL3jHAgEC\n\
-----END DH PARAMETERS-----\n";

static const char file_dh2048[] =
"-----BEGIN DH PARAMETERS-----\n\
MIIBCAKCAQEA9kJXtwh/CBdyorrWqULzBej5UxE5T7bxbrlLOCDaAadWoxTpj0BV\n\
89AHxstDqZSt90xkhkn4DIO9ZekX1KHTUPj1WV/cdlJPPT2N286Z4VeSWc39uK50\n\
T8X8dryDxUcwYc58yWb/Ffm7/ZFexwGq01uejaClcjrUGvC/RgBYK+X0iP1YTknb\n\
zSC0neSRBzZrM2w4DUUdD3yIsxx8Wy2O9vPJI8BD8KVbGI2Ou1WMuF040zT9fBdX\n\
Q6MdGGzeMyEstSr/POGxKUAYEY18hKcKctaGxAMZyAcpesqVDNmWn6vQClCbAkbT\n\
CD1mpF1Bn5x8vYlLIhkmuquiXsNV6TILOwIBAg==\n\
-----END DH PARAMETERS-----\n";

static const char file_dh4096[] =
"-----BEGIN DH PARAMETERS-----\n\
MIICCAKCAgEA+hRyUsFN4VpJ1O8JLcCo/VWr19k3BCgJ4uk+d+KhehjdRqNDNyOQ\n\
l/MOyQNQfWXPeGKmOmIig6Ev/nm6Nf9Z2B1h3R4hExf+zTiHnvVPeRBhjdQi81rt\n\
Xeoh6TNrSBIKIHfUJWBh3va0TxxjQIs6IZOLeVNRLMqzeylWqMf49HsIXqbcokUS\n\
Vt1BkvLdW48j8PPv5DsKRN3tloTxqDJGo9tKvj1Fuk74A+Xda1kNhB7KFlqMyN98\n\
VETEJ6c7KpfOo30mnK30wqw3S8OtaIR/maYX72tGOno2ehFDkq3pnPtEbD2CScxc\n\
alJC+EL7RPk5c/tgeTvCngvc1KZn92Y//EI7G9tPZtylj2b56sHtMftIoYJ9+ODM\n\
sccD5Piz/rejE3Ome8EOOceUSCYAhXn8b3qvxVI1ddd1pED6FHRhFvLrZxFvBEM9\n\
ERRMp5QqOaHJkM+Dxv8Cj6MqrCbfC4u+ZErxodzuusgDgvZiLF22uxMZbobFWyte\n\
OvOzKGtwcTqO/1wV5gKkzu1ZVswVUQd5Gg8lJicwqRWyyNRczDDoG9jVDxmogKTH\n\
AaqLulO7R8Ifa1SwF2DteSGVtgWEN8gDpN3RBmmPTDngyF2DHb5qmpnznwtFKdTL\n\
KWbuHn491xNO25CQWMtem80uKw+pTnisBRF/454n1Jnhub144YRBoN8CAQI=\n\
-----END DH PARAMETERS-----\n";
#endif

/* ------------------------------------------------------------ */
/*			 Procedures common to all secure sessions			*/
/* ------------------------------------------------------------ */

/*
 *	Initialize global context
 */
int
pqsecure_initialize(PGconn *conn)
{
	int			r = 0;

#ifdef USE_SSL
	r = initialize_SSL(conn);
#endif

	return r;
}

/*
 *	Destroy global context
 */
void
pqsecure_destroy(void)
{
#ifdef USE_SSL
	destroy_SSL();
#endif
}

/*
 *	Attempt to negotiate secure session.
 */
PostgresPollingStatusType
pqsecure_open_client(PGconn *conn)
{
#ifdef USE_SSL
	/* First time through? */
	if (conn->ssl == NULL)
	{
		if (!(conn->ssl = SSL_new(SSL_context)) ||
			!SSL_set_app_data(conn->ssl, conn) ||
			!SSL_set_fd(conn->ssl, conn->sock))
		{
			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not establish SSL connection: %s\n"),
							  SSLerrmessage());
			close_SSL(conn);
			return PGRES_POLLING_FAILED;
		}
	}
	/* Begin or continue the actual handshake */
	return open_client_SSL(conn);
#else
	/* shouldn't get here */
	return PGRES_POLLING_FAILED;
#endif
}

/*
 *	Close secure session.
 */
void
pqsecure_close(PGconn *conn)
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
pqsecure_read(PGconn *conn, void *ptr, size_t len)
{
	ssize_t		n;

#ifdef USE_SSL
	if (conn->ssl)
	{
rloop:
		n = SSL_read(conn->ssl, ptr, len);
		switch (SSL_get_error(conn->ssl, n))
		{
			case SSL_ERROR_NONE:
				break;
			case SSL_ERROR_WANT_READ:
				n = 0;
				break;
			case SSL_ERROR_WANT_WRITE:

				/*
				 * Returning 0 here would cause caller to wait for
				 * read-ready, which is not correct since what SSL wants
				 * is wait for write-ready.  The former could get us stuck
				 * in an infinite wait, so don't risk it; busy-loop
				 * instead.
				 */
				goto rloop;
			case SSL_ERROR_SYSCALL:
				{
					char		sebuf[256];

					if (n == -1)
						printfPQExpBuffer(&conn->errorMessage,
								libpq_gettext("SSL SYSCALL error: %s\n"),
						SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					else
					{
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext("SSL SYSCALL error: EOF detected\n"));

						SOCK_ERRNO_SET(ECONNRESET);
						n = -1;
					}
					break;
				}
			case SSL_ERROR_SSL:
				printfPQExpBuffer(&conn->errorMessage,
					  libpq_gettext("SSL error: %s\n"), SSLerrmessage());
				/* fall through */
			case SSL_ERROR_ZERO_RETURN:
				SOCK_ERRNO_SET(ECONNRESET);
				n = -1;
				break;
			default:
				printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("unrecognized SSL error code\n"));
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
pqsecure_write(PGconn *conn, const void *ptr, size_t len)
{
	ssize_t		n;

#ifndef WIN32
	pqsigfunc	oldsighandler = pqsignal(SIGPIPE, SIG_IGN);
#endif

#ifdef USE_SSL
	if (conn->ssl)
	{
		n = SSL_write(conn->ssl, ptr, len);
		switch (SSL_get_error(conn->ssl, n))
		{
			case SSL_ERROR_NONE:
				break;
			case SSL_ERROR_WANT_READ:

				/*
				 * Returning 0 here causes caller to wait for write-ready,
				 * which is not really the right thing, but it's the best
				 * we can do.
				 */
				n = 0;
				break;
			case SSL_ERROR_WANT_WRITE:
				n = 0;
				break;
			case SSL_ERROR_SYSCALL:
				{
					char		sebuf[256];

					if (n == -1)
						printfPQExpBuffer(&conn->errorMessage,
								libpq_gettext("SSL SYSCALL error: %s\n"),
						SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					else
					{
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext("SSL SYSCALL error: EOF detected\n"));
						SOCK_ERRNO_SET(ECONNRESET);
						n = -1;
					}
					break;
				}
			case SSL_ERROR_SSL:
				printfPQExpBuffer(&conn->errorMessage,
					  libpq_gettext("SSL error: %s\n"), SSLerrmessage());
				/* fall through */
			case SSL_ERROR_ZERO_RETURN:
				SOCK_ERRNO_SET(ECONNRESET);
				n = -1;
				break;
			default:
				printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("unrecognized SSL error code\n"));
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
/*						  SSL specific code						*/
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
verify_cb(int ok, X509_STORE_CTX *ctx)
{
	return ok;
}

#ifdef NOT_USED
/*
 *	Verify that common name resolves to peer.
 */
static int
verify_peer(PGconn *conn)
{
	struct hostent *h = NULL;
	struct sockaddr addr;
	struct sockaddr_in *sin;
	socklen_t	len;
	char	  **s;
	unsigned long l;

	/* get the address on the other side of the socket */
	len = sizeof(addr);
	if (getpeername(conn->sock, &addr, &len) == -1)
	{
		char		sebuf[256];

		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("error querying socket: %s\n"),
						SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		return -1;
	}

	/* weird, but legal case */
	if (addr.sa_family == AF_UNIX)
		return 0;

	{
		struct hostent hpstr;
		char		buf[BUFSIZ];
		int			herrno = 0;
		
		/*
		 *	Currently, pqGethostbyname() is used only on platforms that
		 *	don't have getaddrinfo().  If you enable this function,
		 *	you should convert the pqGethostbyname() function call to
		 *	use getaddrinfo().
		 */
		pqGethostbyname(conn->peer_cn, &hpstr, buf, sizeof(buf),
						&h, &herrno);
	}

	/* what do we know about the peer's common name? */
	if (h == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
		libpq_gettext("could not get information about host (%s): %s\n"),
						  conn->peer_cn, hstrerror(h_errno));
		return -1;
	}

	/* does the address match? */
	switch (addr.sa_family)
	{
		case AF_INET:
			sin = (struct sockaddr_in *) & addr;
			for (s = h->h_addr_list; *s != NULL; s++)
			{
				if (!memcmp(&sin->sin_addr.s_addr, *s, h->h_length))
					return 0;
			}
			break;

		default:
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("unsupported protocol\n"));
			return -1;
	}

	/*
	 * the prior test should be definitive, but in practice it sometimes
	 * fails.  So we also check the aliases.
	 */
	for (s = h->h_aliases; *s != NULL; s++)
	{
		if (strcasecmp(conn->peer_cn, *s) == 0)
			return 0;
	}

	/* generate protocol-aware error message */
	switch (addr.sa_family)
	{
		case AF_INET:
			sin = (struct sockaddr_in *) & addr;
			l = ntohl(sin->sin_addr.s_addr);
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext(
											"server common name \"%s\" does not resolve to %ld.%ld.%ld.%ld\n"),
					 conn->peer_cn, (l >> 24) % 0x100, (l >> 16) % 0x100,
							  (l >> 8) % 0x100, l % 0x100);
			break;
		default:
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext(
											"server common name \"%s\" does not resolve to peer address\n"),
							  conn->peer_cn);
	}

	return -1;
}
#endif

/*
 *	Load precomputed DH parameters.
 *
 *	To prevent "downgrade" attacks, we perform a number of checks
 *	to verify that the DBA-generated DH parameters file contains
 *	what we expect it to contain.
 */
static DH  *
load_dh_file(int keylength)
{
#ifdef WIN32
    return NULL;
#else
	char		pwdbuf[BUFSIZ];
	struct passwd pwdstr;
	struct passwd *pwd = NULL;
	FILE	   *fp;
	char		fnbuf[2048];
	DH		   *dh = NULL;
	int			codes;

	if (pqGetpwuid(getuid(), &pwdstr, pwdbuf, sizeof(pwdbuf), &pwd) == 0)
		return NULL;

	/* attempt to open file.  It's not an error if it doesn't exist. */
	snprintf(fnbuf, sizeof fnbuf, "%s/.postgresql/dh%d.pem",
			 pwd->pw_dir, keylength);

	if ((fp = fopen(fnbuf, "r")) == NULL)
		return NULL;

/*	flock(fileno(fp), LOCK_SH); */
	dh = PEM_read_DHparams(fp, NULL, NULL, NULL);
/*	flock(fileno(fp), LOCK_UN); */
	fclose(fp);

	/* is the prime the correct size? */
	if (dh != NULL && 8 * DH_size(dh) < keylength)
		dh = NULL;

	/* make sure the DH parameters are usable */
	if (dh != NULL)
	{
		if (DH_check(dh, &codes))
			return NULL;
		if (codes & DH_CHECK_P_NOT_PRIME)
			return NULL;
		if ((codes & DH_NOT_SUITABLE_GENERATOR) &&
			(codes & DH_CHECK_P_NOT_SAFE_PRIME))
			return NULL;
	}

	return dh;
#endif
}

/*
 *	Load hardcoded DH parameters.
 *
 *	To prevent problems if the DH parameters files don't even
 *	exist, we can load DH parameters hardcoded into this file.
 */
static DH  *
load_dh_buffer(const char *buffer, size_t len)
{
	BIO		   *bio;
	DH		   *dh = NULL;

	bio = BIO_new_mem_buf((char *) buffer, len);
	if (bio == NULL)
		return NULL;
	dh = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
	BIO_free(bio);

	return dh;
}

/*
 *	Generate an empheral DH key.  Because this can take a long
 *	time to compute, we can use precomputed parameters of the
 *	common key sizes.
 *
 *	Since few sites will bother to precompute these parameter
 *	files, we also provide a fallback to the parameters provided
 *	by the OpenSSL project.
 *
 *	These values can be static (once loaded or computed) since
 *	the OpenSSL library can efficiently generate random keys from
 *	the information provided.
 */
static DH  *
tmp_dh_cb(SSL *s, int is_export, int keylength)
{
	DH		   *r = NULL;
	static DH  *dh = NULL;
	static DH  *dh512 = NULL;
	static DH  *dh1024 = NULL;
	static DH  *dh2048 = NULL;
	static DH  *dh4096 = NULL;

	switch (keylength)
	{
		case 512:
			if (dh512 == NULL)
				dh512 = load_dh_file(keylength);
			if (dh512 == NULL)
				dh512 = load_dh_buffer(file_dh512, sizeof file_dh512);
			r = dh512;
			break;

		case 1024:
			if (dh1024 == NULL)
				dh1024 = load_dh_file(keylength);
			if (dh1024 == NULL)
				dh1024 = load_dh_buffer(file_dh1024, sizeof file_dh1024);
			r = dh1024;
			break;

		case 2048:
			if (dh2048 == NULL)
				dh2048 = load_dh_file(keylength);
			if (dh2048 == NULL)
				dh2048 = load_dh_buffer(file_dh2048, sizeof file_dh2048);
			r = dh2048;
			break;

		case 4096:
			if (dh4096 == NULL)
				dh4096 = load_dh_file(keylength);
			if (dh4096 == NULL)
				dh4096 = load_dh_buffer(file_dh4096, sizeof file_dh4096);
			r = dh4096;
			break;

		default:
			if (dh == NULL)
				dh = load_dh_file(keylength);
			r = dh;
	}

	/* this may take a long time, but it may be necessary... */
	if (r == NULL || 8 * DH_size(r) < keylength)
		r = DH_generate_parameters(keylength, DH_GENERATOR_2, NULL, NULL);

	return r;
}

/*
 *	Callback used by SSL to load client cert and key.
 *	This callback is only called when the server wants a
 *	client cert.
 *
 *	Returns 1 on success, 0 on no data, -1 on error.
 */
static int
client_cert_cb(SSL *ssl, X509 **x509, EVP_PKEY **pkey)
{
#ifdef WIN32
   return 0;
#else
	char		pwdbuf[BUFSIZ];
	struct passwd pwdstr;
	struct passwd *pwd = NULL;
	struct stat buf,
				buf2;
	char		fnbuf[2048];
	FILE	   *fp;
	PGconn	   *conn = (PGconn *) SSL_get_app_data(ssl);
	int			(*cb) () = NULL;	/* how to read user password */
	char		sebuf[256];


	if (pqGetpwuid(getuid(), &pwdstr, pwdbuf, sizeof(pwdbuf), &pwd) == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
					  libpq_gettext("could not get user information\n"));
		return -1;
	}

	/* read the user certificate */
	snprintf(fnbuf, sizeof fnbuf, "%s/.postgresql/postgresql.crt",
			 pwd->pw_dir);
	if (stat(fnbuf, &buf) == -1)
		return 0;
	if ((fp = fopen(fnbuf, "r")) == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
				  libpq_gettext("could not open certificate (%s): %s\n"),
						  fnbuf, pqStrerror(errno, sebuf, sizeof(sebuf)));
		return -1;
	}
	if (PEM_read_X509(fp, x509, NULL, NULL) == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
				  libpq_gettext("could not read certificate (%s): %s\n"),
						  fnbuf, SSLerrmessage());
		fclose(fp);
		return -1;
	}
	fclose(fp);

	/* read the user key */
	snprintf(fnbuf, sizeof fnbuf, "%s/.postgresql/postgresql.key",
			 pwd->pw_dir);
	if (stat(fnbuf, &buf) == -1)
	{
		printfPQExpBuffer(&conn->errorMessage,
		libpq_gettext("certificate present, but not private key (%s)\n"),
						  fnbuf);
		X509_free(*x509);
		return 0;
	}
	if (!S_ISREG(buf.st_mode) || (buf.st_mode & 0077) ||
		buf.st_uid != getuid())
	{
		printfPQExpBuffer(&conn->errorMessage,
		libpq_gettext("private key (%s) has wrong permissions\n"), fnbuf);
		X509_free(*x509);
		return -1;
	}
	if ((fp = fopen(fnbuf, "r")) == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
			 libpq_gettext("could not open private key file (%s): %s\n"),
						  fnbuf, pqStrerror(errno, sebuf, sizeof(sebuf)));
		X509_free(*x509);
		return -1;
	}
	if (fstat(fileno(fp), &buf2) == -1 ||
		buf.st_dev != buf2.st_dev || buf.st_ino != buf2.st_ino)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("private key (%s) changed during execution\n"), fnbuf);
		X509_free(*x509);
		return -1;
	}
	if (PEM_read_PrivateKey(fp, pkey, cb, NULL) == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
				  libpq_gettext("could not read private key (%s): %s\n"),
						  fnbuf, SSLerrmessage());
		X509_free(*x509);
		fclose(fp);
		return -1;
	}
	fclose(fp);

	/* verify that the cert and key go together */
	if (!X509_check_private_key(*x509, *pkey))
	{
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("certificate/private key mismatch (%s): %s\n"),
						  fnbuf, SSLerrmessage());
		X509_free(*x509);
		EVP_PKEY_free(*pkey);
		return -1;
	}

	return 1;
#endif
}

/*
 *	Initialize global SSL context.
 */
static int
initialize_SSL(PGconn *conn)
{
#ifndef WIN32
	struct stat buf;
	char		pwdbuf[BUFSIZ];
	struct passwd pwdstr;
	struct passwd *pwd = NULL;
	char		fnbuf[2048];
#endif

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

#ifndef WIN32
	if (pqGetpwuid(getuid(), &pwdstr, pwdbuf, sizeof(pwdbuf), &pwd) == 0)
	{
		snprintf(fnbuf, sizeof fnbuf, "%s/.postgresql/root.crt",
				 pwd->pw_dir);
		if (stat(fnbuf, &buf) == -1)
		{
			return 0;
#ifdef NOT_USED
			char		sebuf[256];

			/* CLIENT CERTIFICATES NOT REQUIRED  bjm 2002-09-26 */
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("could not read root certificate list (%s): %s\n"),
						 fnbuf, pqStrerror(errno, sebuf, sizeof(sebuf)));
			return -1;
#endif
		}
		if (!SSL_CTX_load_verify_locations(SSL_context, fnbuf, 0))
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("could not read root certificate list (%s): %s\n"),
							  fnbuf, SSLerrmessage());
			return -1;
		}
	}

	SSL_CTX_set_verify(SSL_context,
		   SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify_cb);
	SSL_CTX_set_verify_depth(SSL_context, 1);

	/* set up empheral DH keys */
	SSL_CTX_set_tmp_dh_callback(SSL_context, tmp_dh_cb);
	SSL_CTX_set_options(SSL_context, SSL_OP_SINGLE_DH_USE);

	/* set up mechanism to provide client certificate, if available */
	SSL_CTX_set_client_cert_cb(SSL_context, client_cert_cb);
#endif

	return 0;
}

/*
 *	Destroy global SSL context.
 */
static void
destroy_SSL(void)
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
static PostgresPollingStatusType
open_client_SSL(PGconn *conn)
{
	int			r;

	r = SSL_connect(conn->ssl);
	if (r <= 0)
	{
		switch (SSL_get_error(conn->ssl, r))
		{
			case SSL_ERROR_WANT_READ:
				return PGRES_POLLING_READING;

			case SSL_ERROR_WANT_WRITE:
				return PGRES_POLLING_WRITING;

			case SSL_ERROR_SYSCALL:
				{
					char		sebuf[256];

					if (r == -1)
						printfPQExpBuffer(&conn->errorMessage,
								libpq_gettext("SSL SYSCALL error: %s\n"),
						SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					else
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext("SSL SYSCALL error: EOF detected\n"));
					close_SSL(conn);
					return PGRES_POLLING_FAILED;
				}
			case SSL_ERROR_SSL:
				printfPQExpBuffer(&conn->errorMessage,
					  libpq_gettext("SSL error: %s\n"), SSLerrmessage());
				close_SSL(conn);
				return PGRES_POLLING_FAILED;

			default:
				printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("unrecognized SSL error code\n"));
				close_SSL(conn);
				return PGRES_POLLING_FAILED;
		}
	}

	/* check the certificate chain of the server */

#ifdef NOT_USED
	/* CLIENT CERTIFICATES NOT REQUIRED  bjm 2002-09-26 */

	/*
	 * this eliminates simple man-in-the-middle attacks and simple
	 * impersonations
	 */
	r = SSL_get_verify_result(conn->ssl);
	if (r != X509_V_OK)
	{
		printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("certificate could not be validated: %s\n"),
						  X509_verify_cert_error_string(r));
		close_SSL(conn);
		return PGRES_POLLING_FAILED;
	}
#endif

	/* pull out server distinguished and common names */
	conn->peer = SSL_get_peer_certificate(conn->ssl);
	if (conn->peer == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("certificate could not be obtained: %s\n"),
						  SSLerrmessage());
		close_SSL(conn);
		return PGRES_POLLING_FAILED;
	}

	X509_NAME_oneline(X509_get_subject_name(conn->peer),
					  conn->peer_dn, sizeof(conn->peer_dn));
	conn->peer_dn[sizeof(conn->peer_dn) - 1] = '\0';

	X509_NAME_get_text_by_NID(X509_get_subject_name(conn->peer),
							  NID_commonName, conn->peer_cn, SM_USER);
	conn->peer_cn[SM_USER] = '\0';

	/* verify that the common name resolves to peer */

#ifdef NOT_USED
	/* CLIENT CERTIFICATES NOT REQUIRED  bjm 2002-09-26 */

	/*
	 * this is necessary to eliminate man-in-the-middle attacks and
	 * impersonations where the attacker somehow learned the server's
	 * private key
	 */
	if (verify_peer(conn) == -1)
	{
		close_SSL(conn);
		return PGRES_POLLING_FAILED;
	}
#endif

	/* SSL handshake is complete */
	return PGRES_POLLING_OK;
}

/*
 *	Close SSL connection.
 */
static void
close_SSL(PGconn *conn)
{
	if (conn->ssl)
	{
		SSL_shutdown(conn->ssl);
		SSL_free(conn->ssl);
		conn->ssl = NULL;
	}

	if (conn->peer)
	{
		X509_free(conn->peer);
		conn->peer = NULL;
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
	unsigned long errcode;
	const char *errreason;
	static char errbuf[32];

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

#endif   /* USE_SSL */
