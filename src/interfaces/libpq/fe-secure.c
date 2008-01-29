/*-------------------------------------------------------------------------
 *
 * fe-secure.c
 *	  functions related to setting up a secure connection to the backend.
 *	  Secure connections are expected to provide confidentiality,
 *	  message integrity and endpoint authentication.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/interfaces/libpq/fe-secure.c,v 1.102 2008/01/29 02:03:39 tgl Exp $
 *
 * NOTES
 *	  [ Most of these notes are wrong/obsolete, but perhaps not all ]
 *
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
 *	  "~/.postgresql/root.crt" file.  If this file isn't
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
 *	  static private key (~/.postgresql/postgresql.key)
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
 *		~/.postgresql/postgresql.crt
 *	  and
 *		~/.postgresql/postgresql.key
 *	  respectively.
 *
 *	  ...
 *
 *	  We don't provide informational callbacks here (like
 *	  info_cb() in be-secure.c), since there's mechanism to
 *	  display that information to the client.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <signal.h>
#include <fcntl.h>
#include <ctype.h>

#include "libpq-fe.h"
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
#include <sys/stat.h>

#ifdef ENABLE_THREAD_SAFETY
#ifdef WIN32
#include "pthread-win32.h"
#else
#include <pthread.h>
#endif
#endif

#ifdef USE_SSL
#include <openssl/ssl.h>
#include <openssl/bio.h>
#if (SSLEAY_VERSION_NUMBER >= 0x00907000L)
#include <openssl/conf.h>
#endif
#if (SSLEAY_VERSION_NUMBER >= 0x00907000L) && !defined(OPENSSL_NO_ENGINE)
#include <openssl/engine.h>
#endif
#endif   /* USE_SSL */


#ifdef USE_SSL

#ifndef WIN32
#define USER_CERT_FILE		".postgresql/postgresql.crt"
#define USER_KEY_FILE		".postgresql/postgresql.key"
#define ROOT_CERT_FILE		".postgresql/root.crt"
#define ROOT_CRL_FILE		".postgresql/root.crl"
#else
/* On Windows, the "home" directory is already PostgreSQL-specific */
#define USER_CERT_FILE		"postgresql.crt"
#define USER_KEY_FILE		"postgresql.key"
#define ROOT_CERT_FILE		"root.crt"
#define ROOT_CRL_FILE		"root.crl"
#endif

#ifndef HAVE_ERR_SET_MARK
/* These don't exist in OpenSSL before 0.9.8 */
#define ERR_set_mark()		((void) 0)
#define ERR_pop_to_mark()	((void) 0)
#endif

#ifdef NOT_USED
static int	verify_peer(PGconn *);
#endif
static int	verify_cb(int ok, X509_STORE_CTX *ctx);
static int	client_cert_cb(SSL *, X509 **, EVP_PKEY **);
static int	init_ssl_system(PGconn *conn);
static int	initialize_SSL(PGconn *);
static void destroy_SSL(void);
static PostgresPollingStatusType open_client_SSL(PGconn *);
static void close_SSL(PGconn *);
static char *SSLerrmessage(void);
static void SSLerrfree(char *buf);
#endif

#ifdef USE_SSL
static bool pq_initssllib = true;

static SSL_CTX *SSL_context = NULL;
#endif

/*
 * Macros to handle disabling and then restoring the state of SIGPIPE handling.
 * Note that DISABLE_SIGPIPE() must appear at the start of a block.
 */

#ifndef WIN32
#ifdef ENABLE_THREAD_SAFETY

#define DISABLE_SIGPIPE(failaction) \
	sigset_t	osigmask; \
	bool		sigpipe_pending; \
	bool		got_epipe = false; \
\
	if (pq_block_sigpipe(&osigmask, &sigpipe_pending) < 0) \
		failaction

#define REMEMBER_EPIPE(cond) \
	do { \
		if (cond) \
			got_epipe = true; \
	} while (0)

#define RESTORE_SIGPIPE() \
	pq_reset_sigpipe(&osigmask, sigpipe_pending, got_epipe)

#else	/* !ENABLE_THREAD_SAFETY */

#define DISABLE_SIGPIPE(failaction) \
	pqsigfunc	oldsighandler = pqsignal(SIGPIPE, SIG_IGN)

#define REMEMBER_EPIPE(cond)

#define RESTORE_SIGPIPE() \
	pqsignal(SIGPIPE, oldsighandler)

#endif	/* ENABLE_THREAD_SAFETY */
#else	/* WIN32 */

#define DISABLE_SIGPIPE(failaction)
#define REMEMBER_EPIPE(cond)
#define RESTORE_SIGPIPE()

#endif	/* WIN32 */

/* ------------------------------------------------------------ */
/*			 Procedures common to all secure sessions			*/
/* ------------------------------------------------------------ */


/*
 *	Exported function to allow application to tell us it's already
 *	initialized OpenSSL.
 */
void
PQinitSSL(int do_init)
{
#ifdef USE_SSL
	pq_initssllib = do_init;
#endif
}

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
			char	   *err = SSLerrmessage();

			printfPQExpBuffer(&conn->errorMessage,
				   libpq_gettext("could not establish SSL connection: %s\n"),
							  err);
			SSLerrfree(err);
			close_SSL(conn);
			return PGRES_POLLING_FAILED;
		}

		/*
		 * Initialize errorMessage to empty.  This allows open_client_SSL() to
		 * detect whether client_cert_cb() has stored a message.
		 */
		resetPQExpBuffer(&conn->errorMessage);
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
		int			err;

		/* SSL_read can write to the socket, so we need to disable SIGPIPE */
		DISABLE_SIGPIPE(return -1);

rloop:
		n = SSL_read(conn->ssl, ptr, len);
		err = SSL_get_error(conn->ssl, n);
		switch (err)
		{
			case SSL_ERROR_NONE:
				break;
			case SSL_ERROR_WANT_READ:
				n = 0;
				break;
			case SSL_ERROR_WANT_WRITE:

				/*
				 * Returning 0 here would cause caller to wait for read-ready,
				 * which is not correct since what SSL wants is wait for
				 * write-ready.  The former could get us stuck in an infinite
				 * wait, so don't risk it; busy-loop instead.
				 */
				goto rloop;
			case SSL_ERROR_SYSCALL:
				{
					char		sebuf[256];

					if (n == -1)
					{
						REMEMBER_EPIPE(SOCK_ERRNO == EPIPE);
						printfPQExpBuffer(&conn->errorMessage,
									libpq_gettext("SSL SYSCALL error: %s\n"),
							SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					}
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
				{
					char	   *err = SSLerrmessage();

					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("SSL error: %s\n"), err);
					SSLerrfree(err);
				}
				/* fall through */
			case SSL_ERROR_ZERO_RETURN:
				SOCK_ERRNO_SET(ECONNRESET);
				n = -1;
				break;
			default:
				printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("unrecognized SSL error code: %d\n"),
								  err);
				n = -1;
				break;
		}

		RESTORE_SIGPIPE();
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

	DISABLE_SIGPIPE(return -1);

#ifdef USE_SSL
	if (conn->ssl)
	{
		int			err;

		n = SSL_write(conn->ssl, ptr, len);
		err = SSL_get_error(conn->ssl, n);
		switch (err)
		{
			case SSL_ERROR_NONE:
				break;
			case SSL_ERROR_WANT_READ:

				/*
				 * Returning 0 here causes caller to wait for write-ready,
				 * which is not really the right thing, but it's the best we
				 * can do.
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
					{
						REMEMBER_EPIPE(SOCK_ERRNO == EPIPE);
						printfPQExpBuffer(&conn->errorMessage,
									libpq_gettext("SSL SYSCALL error: %s\n"),
							SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					}
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
				{
					char	   *err = SSLerrmessage();

					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("SSL error: %s\n"), err);
					SSLerrfree(err);
				}
				/* fall through */
			case SSL_ERROR_ZERO_RETURN:
				SOCK_ERRNO_SET(ECONNRESET);
				n = -1;
				break;
			default:
				printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("unrecognized SSL error code: %d\n"),
								  err);
				n = -1;
				break;
		}
	}
	else
#endif
	{
		n = send(conn->sock, ptr, len, 0);
		REMEMBER_EPIPE(n < 0 && SOCK_ERRNO == EPIPE);
	}

	RESTORE_SIGPIPE();

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
	ACCEPT_TYPE_ARG3 len;
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
		 * Currently, pqGethostbyname() is used only on platforms that don't
		 * have getaddrinfo().	If you enable this function, you should
		 * convert the pqGethostbyname() function call to use getaddrinfo().
		 */
		pqGethostbyname(conn->peer_cn, &hpstr, buf, sizeof(buf),
						&h, &herrno);
	}

	/* what do we know about the peer's common name? */
	if (h == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
		  libpq_gettext("could not get information about host \"%s\": %s\n"),
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
		if (pg_strcasecmp(conn->peer_cn, *s) == 0)
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
#endif   /* NOT_USED */

/*
 *	Callback used by SSL to load client cert and key.
 *	This callback is only called when the server wants a
 *	client cert.
 *
 *	Since BIO functions can set OpenSSL error codes, we must
 *	reset the OpenSSL error stack on *every* exit from this
 *	function once we've started using BIO.
 *
 *	Must return 1 on success, 0 on no data or error.
 */
static int
client_cert_cb(SSL *ssl, X509 **x509, EVP_PKEY **pkey)
{
	char		homedir[MAXPGPATH];
	struct stat buf;

#ifndef WIN32
	struct stat buf2;
	FILE	   *fp;
#endif
	char		fnbuf[MAXPGPATH];
	BIO		   *bio;
	PGconn	   *conn = (PGconn *) SSL_get_app_data(ssl);
	char		sebuf[256];

	if (!pqGetHomeDirectory(homedir, sizeof(homedir)))
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("could not get user information\n"));
		return 0;
	}

	/* read the user certificate */
	snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, USER_CERT_FILE);

	/*
	 * OpenSSL <= 0.9.8 lacks error stack handling, which means it's likely to
	 * report wrong error messages if access to the cert file fails. Do our
	 * own check for the readability of the file to catch the majority of such
	 * problems before OpenSSL gets involved.
	 */
#ifndef HAVE_ERR_SET_MARK
	{
		FILE	   *fp2;

		if ((fp2 = fopen(fnbuf, "r")) == NULL)
		{
			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not open certificate file \"%s\": %s\n"),
							  fnbuf, pqStrerror(errno, sebuf, sizeof(sebuf)));
			return 0;
		}
		fclose(fp2);
	}
#endif

	/* save OpenSSL error stack */
	ERR_set_mark();

	if ((bio = BIO_new_file(fnbuf, "r")) == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not open certificate file \"%s\": %s\n"),
						  fnbuf, pqStrerror(errno, sebuf, sizeof(sebuf)));
		ERR_pop_to_mark();
		return 0;
	}

	if (PEM_read_bio_X509(bio, x509, NULL, NULL) == NULL)
	{
		char	   *err = SSLerrmessage();

		printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not read certificate file \"%s\": %s\n"),
						  fnbuf, err);
		SSLerrfree(err);
		BIO_free(bio);
		ERR_pop_to_mark();
		return 0;
	}

	BIO_free(bio);

#if (SSLEAY_VERSION_NUMBER >= 0x00907000L) && !defined(OPENSSL_NO_ENGINE)
	if (getenv("PGSSLKEY"))
	{
		/* read the user key from engine */
		char	   *engine_env = getenv("PGSSLKEY");
		char	   *engine_colon = strchr(engine_env, ':');
		char	   *engine_str;
		ENGINE	   *engine_ptr;

		if (!engine_colon)
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("invalid value of PGSSLKEY environment variable\n"));
			ERR_pop_to_mark();
			return 0;
		}

		engine_str = malloc(engine_colon - engine_env + 1);
		strlcpy(engine_str, engine_env, engine_colon - engine_env + 1);
		engine_ptr = ENGINE_by_id(engine_str);
		if (engine_ptr == NULL)
		{
			char	   *err = SSLerrmessage();

			printfPQExpBuffer(&conn->errorMessage,
					 libpq_gettext("could not load SSL engine \"%s\": %s\n"),
							  engine_str, err);
			SSLerrfree(err);
			free(engine_str);
			ERR_pop_to_mark();
			return 0;
		}

		*pkey = ENGINE_load_private_key(engine_ptr, engine_colon + 1,
										NULL, NULL);
		if (*pkey == NULL)
		{
			char	   *err = SSLerrmessage();

			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("could not read private SSL key \"%s\" from engine \"%s\": %s\n"),
							  engine_colon + 1, engine_str, err);
			SSLerrfree(err);
			free(engine_str);
			ERR_pop_to_mark();
			return 0;
		}
		free(engine_str);
	}
	else
#endif   /* use PGSSLKEY */
	{
		/* read the user key from file */
		snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, USER_KEY_FILE);
		if (stat(fnbuf, &buf) == -1)
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("certificate present, but not private key file \"%s\"\n"),
							  fnbuf);
			ERR_pop_to_mark();
			return 0;
		}
#ifndef WIN32
		if (!S_ISREG(buf.st_mode) || (buf.st_mode & 0077) ||
			buf.st_uid != geteuid())
		{
			printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("private key file \"%s\" has wrong permissions\n"),
							  fnbuf);
			ERR_pop_to_mark();
			return 0;
		}
#endif

		if ((bio = BIO_new_file(fnbuf, "r")) == NULL)
		{
			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not open private key file \"%s\": %s\n"),
							  fnbuf, pqStrerror(errno, sebuf, sizeof(sebuf)));
			ERR_pop_to_mark();
			return 0;
		}
#ifndef WIN32
		BIO_get_fp(bio, &fp);
		if (fstat(fileno(fp), &buf2) == -1 ||
			buf.st_dev != buf2.st_dev || buf.st_ino != buf2.st_ino)
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("private key file \"%s\" changed during execution\n"), fnbuf);
			ERR_pop_to_mark();
			return 0;
		}
#endif

		if (PEM_read_bio_PrivateKey(bio, pkey, NULL, NULL) == NULL)
		{
			char	   *err = SSLerrmessage();

			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not read private key file \"%s\": %s\n"),
							  fnbuf, err);
			SSLerrfree(err);

			BIO_free(bio);
			ERR_pop_to_mark();
			return 0;
		}

		BIO_free(bio);
	}

	/* verify that the cert and key go together */
	if (!X509_check_private_key(*x509, *pkey))
	{
		char	   *err = SSLerrmessage();

		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("certificate does not match private key file \"%s\": %s\n"),
						  fnbuf, err);
		SSLerrfree(err);
		ERR_pop_to_mark();
		return 0;
	}

	ERR_pop_to_mark();

	return 1;
}

#ifdef ENABLE_THREAD_SAFETY

static unsigned long
pq_threadidcallback(void)
{
	/*
	 * This is not standards-compliant.  pthread_self() returns pthread_t, and
	 * shouldn't be cast to unsigned long, but CRYPTO_set_id_callback requires
	 * it, so we have to do it.
	 */
	return (unsigned long) pthread_self();
}

static pthread_mutex_t *pq_lockarray;

static void
pq_lockingcallback(int mode, int n, const char *file, int line)
{
	if (mode & CRYPTO_LOCK)
		pthread_mutex_lock(&pq_lockarray[n]);
	else
		pthread_mutex_unlock(&pq_lockarray[n]);
}
#endif   /* ENABLE_THREAD_SAFETY */

static int
init_ssl_system(PGconn *conn)
{
#ifdef ENABLE_THREAD_SAFETY
#ifndef WIN32
	static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
#else
	static pthread_mutex_t init_mutex = NULL;
	static long mutex_initlock = 0;

	if (init_mutex == NULL)
	{
		while (InterlockedExchange(&mutex_initlock, 1) == 1)
			 /* loop, another thread own the lock */ ;
		if (init_mutex == NULL)
			pthread_mutex_init(&init_mutex, NULL);
		InterlockedExchange(&mutex_initlock, 0);
	}
#endif
	pthread_mutex_lock(&init_mutex);

	if (pq_initssllib && pq_lockarray == NULL)
	{
		int			i;

		CRYPTO_set_id_callback(pq_threadidcallback);

		pq_lockarray = malloc(sizeof(pthread_mutex_t) * CRYPTO_num_locks());
		if (!pq_lockarray)
		{
			pthread_mutex_unlock(&init_mutex);
			return -1;
		}
		for (i = 0; i < CRYPTO_num_locks(); i++)
			pthread_mutex_init(&pq_lockarray[i], NULL);

		CRYPTO_set_locking_callback(pq_lockingcallback);
	}
#endif
	if (!SSL_context)
	{
		if (pq_initssllib)
		{
#if SSLEAY_VERSION_NUMBER >= 0x00907000L
			OPENSSL_config(NULL);
#endif
			SSL_library_init();
			SSL_load_error_strings();
		}
		SSL_context = SSL_CTX_new(TLSv1_method());
		if (!SSL_context)
		{
			char	   *err = SSLerrmessage();

			printfPQExpBuffer(&conn->errorMessage,
						 libpq_gettext("could not create SSL context: %s\n"),
							  err);
			SSLerrfree(err);
#ifdef ENABLE_THREAD_SAFETY
			pthread_mutex_unlock(&init_mutex);
#endif
			return -1;
		}
	}
#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&init_mutex);
#endif
	return 0;
}

/*
 *	Initialize global SSL context.
 */
static int
initialize_SSL(PGconn *conn)
{
	struct stat buf;
	char		homedir[MAXPGPATH];
	char		fnbuf[MAXPGPATH];

	if (init_ssl_system(conn))
		return -1;

	/* Set up to verify server cert, if root.crt is present */
	if (pqGetHomeDirectory(homedir, sizeof(homedir)))
	{
		snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, ROOT_CERT_FILE);
		if (stat(fnbuf, &buf) == 0)
		{
			X509_STORE *cvstore;

			if (!SSL_CTX_load_verify_locations(SSL_context, fnbuf, NULL))
			{
				char	   *err = SSLerrmessage();

				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("could not read root certificate file \"%s\": %s\n"),
								  fnbuf, err);
				SSLerrfree(err);
				return -1;
			}

			if ((cvstore = SSL_CTX_get_cert_store(SSL_context)) != NULL)
			{
				/* setting the flags to check against the complete CRL chain */
				if (X509_STORE_load_locations(cvstore, ROOT_CRL_FILE, NULL) != 0)
/* OpenSSL 0.96 does not support X509_V_FLAG_CRL_CHECK */
#ifdef X509_V_FLAG_CRL_CHECK
					X509_STORE_set_flags(cvstore,
						  X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
				/* if not found, silently ignore;  we do not require CRL */
#else
				{
					char	   *err = SSLerrmessage();

					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("SSL library does not support CRL certificates (file \"%s\")\n"),
									  fnbuf);
					SSLerrfree(err);
					return -1;
				}
#endif
			}

			SSL_CTX_set_verify(SSL_context, SSL_VERIFY_PEER, verify_cb);
		}
	}

	/* set up mechanism to provide client certificate, if available */
	SSL_CTX_set_client_cert_cb(SSL_context, client_cert_cb);

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
		int			err = SSL_get_error(conn->ssl, r);

		switch (err)
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
				{
					/*
					 * If there are problems with the local certificate files,
					 * these will be detected by client_cert_cb() which is
					 * called from SSL_connect().  We want to return that
					 * error message and not the rather unhelpful error that
					 * OpenSSL itself returns.	So check to see if an error
					 * message was already stored.
					 */
					if (conn->errorMessage.len == 0)
					{
						char	   *err = SSLerrmessage();

						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext("SSL error: %s\n"),
										  err);
						SSLerrfree(err);
					}
					close_SSL(conn);
					return PGRES_POLLING_FAILED;
				}

			default:
				printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("unrecognized SSL error code: %d\n"),
								  err);
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
		char	   *err = SSLerrmessage();

		printfPQExpBuffer(&conn->errorMessage,
					libpq_gettext("certificate could not be obtained: %s\n"),
						  err);
		SSLerrfree(err);
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
	 * impersonations where the attacker somehow learned the server's private
	 * key
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
		DISABLE_SIGPIPE((void) 0);
		SSL_shutdown(conn->ssl);
		SSL_free(conn->ssl);
		conn->ssl = NULL;
		/* We have to assume we got EPIPE */
		REMEMBER_EPIPE(true);
		RESTORE_SIGPIPE();
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
static char ssl_nomem[] = "out of memory allocating error description";

#define SSL_ERR_LEN 128

static char *
SSLerrmessage(void)
{
	unsigned long errcode;
	const char *errreason;
	char	   *errbuf;

	errbuf = malloc(SSL_ERR_LEN);
	if (!errbuf)
		return ssl_nomem;
	errcode = ERR_get_error();
	if (errcode == 0)
	{
		snprintf(errbuf, SSL_ERR_LEN, libpq_gettext("no SSL error reported"));
		return errbuf;
	}
	errreason = ERR_reason_error_string(errcode);
	if (errreason != NULL)
	{
		strlcpy(errbuf, errreason, SSL_ERR_LEN);
		return errbuf;
	}
	snprintf(errbuf, SSL_ERR_LEN, libpq_gettext("SSL error code %lu"), errcode);
	return errbuf;
}

static void
SSLerrfree(char *buf)
{
	if (buf != ssl_nomem)
		free(buf);
}

/*
 *	Return pointer to OpenSSL object.
 */
void *
PQgetssl(PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->ssl;
}
#else							/* !USE_SSL */

void *
PQgetssl(PGconn *conn)
{
	return NULL;
}
#endif   /* USE_SSL */


#if defined(ENABLE_THREAD_SAFETY) && !defined(WIN32)

/*
 *	Block SIGPIPE for this thread.	This prevents send()/write() from exiting
 *	the application.
 */
int
pq_block_sigpipe(sigset_t *osigset, bool *sigpipe_pending)
{
	sigset_t	sigpipe_sigset;
	sigset_t	sigset;

	sigemptyset(&sigpipe_sigset);
	sigaddset(&sigpipe_sigset, SIGPIPE);

	/* Block SIGPIPE and save previous mask for later reset */
	SOCK_ERRNO_SET(pthread_sigmask(SIG_BLOCK, &sigpipe_sigset, osigset));
	if (SOCK_ERRNO)
		return -1;

	/* We can have a pending SIGPIPE only if it was blocked before */
	if (sigismember(osigset, SIGPIPE))
	{
		/* Is there a pending SIGPIPE? */
		if (sigpending(&sigset) != 0)
			return -1;

		if (sigismember(&sigset, SIGPIPE))
			*sigpipe_pending = true;
		else
			*sigpipe_pending = false;
	}
	else
		*sigpipe_pending = false;

	return 0;
}

/*
 *	Discard any pending SIGPIPE and reset the signal mask.
 *
 * Note: we are effectively assuming here that the C library doesn't queue
 * up multiple SIGPIPE events.	If it did, then we'd accidentally leave
 * ours in the queue when an event was already pending and we got another.
 * As long as it doesn't queue multiple events, we're OK because the caller
 * can't tell the difference.
 *
 * The caller should say got_epipe = FALSE if it is certain that it
 * didn't get an EPIPE error; in that case we'll skip the clear operation
 * and things are definitely OK, queuing or no.  If it got one or might have
 * gotten one, pass got_epipe = TRUE.
 *
 * We do not want this to change errno, since if it did that could lose
 * the error code from a preceding send().	We essentially assume that if
 * we were able to do pq_block_sigpipe(), this can't fail.
 */
void
pq_reset_sigpipe(sigset_t *osigset, bool sigpipe_pending, bool got_epipe)
{
	int			save_errno = SOCK_ERRNO;
	int			signo;
	sigset_t	sigset;

	/* Clear SIGPIPE only if none was pending */
	if (got_epipe && !sigpipe_pending)
	{
		if (sigpending(&sigset) == 0 &&
			sigismember(&sigset, SIGPIPE))
		{
			sigset_t	sigpipe_sigset;

			sigemptyset(&sigpipe_sigset);
			sigaddset(&sigpipe_sigset, SIGPIPE);

			sigwait(&sigpipe_sigset, &signo);
		}
	}

	/* Restore saved block mask */
	pthread_sigmask(SIG_SETMASK, osigset, NULL);

	SOCK_ERRNO_SET(save_errno);
}

#endif   /* ENABLE_THREAD_SAFETY && !WIN32 */
