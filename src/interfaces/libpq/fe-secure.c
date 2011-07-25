/*-------------------------------------------------------------------------
 *
 * fe-secure.c
 *	  functions related to setting up a secure connection to the backend.
 *	  Secure connections are expected to provide confidentiality,
 *	  message integrity and endpoint authentication.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-secure.c
 *
 * NOTES
 *
 *	  We don't provide informational callbacks here (like
 *	  info_cb() in be-secure.c), since there's no good mechanism to
 *	  display such information to the user.
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
#include "libpq-int.h"

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
#if (SSLEAY_VERSION_NUMBER >= 0x00907000L)
#include <openssl/conf.h>
#endif
#ifdef USE_SSL_ENGINE
#include <openssl/engine.h>
#endif


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

static bool verify_peer_name_matches_certificate(PGconn *);
static int	verify_cb(int ok, X509_STORE_CTX *ctx);
static int	init_ssl_system(PGconn *conn);
static void destroy_ssl_system(void);
static int	initialize_SSL(PGconn *conn);
static void destroySSL(void);
static PostgresPollingStatusType open_client_SSL(PGconn *);
static void close_SSL(PGconn *);
static char *SSLerrmessage(void);
static void SSLerrfree(char *buf);

static bool pq_init_ssl_lib = true;
static bool pq_init_crypto_lib = true;
static SSL_CTX *SSL_context = NULL;

#ifdef ENABLE_THREAD_SAFETY
static long ssl_open_connections = 0;

#ifndef WIN32
static pthread_mutex_t ssl_config_mutex = PTHREAD_MUTEX_INITIALIZER;
#else
static pthread_mutex_t ssl_config_mutex = NULL;
static long win32_ssl_create_mutex = 0;
#endif
#endif   /* ENABLE_THREAD_SAFETY */
#endif   /* SSL */


/*
 * Macros to handle disabling and then restoring the state of SIGPIPE handling.
 * On Windows, these are all no-ops since there's no SIGPIPEs.
 */

#ifndef WIN32

#define SIGPIPE_MASKED(conn)	((conn)->sigpipe_so || (conn)->sigpipe_flag)

#ifdef ENABLE_THREAD_SAFETY

struct sigpipe_info
{
	sigset_t	oldsigmask;
	bool		sigpipe_pending;
	bool		got_epipe;
};

#define DECLARE_SIGPIPE_INFO(spinfo) struct sigpipe_info spinfo

#define DISABLE_SIGPIPE(conn, spinfo, failaction) \
	do { \
		(spinfo).got_epipe = false; \
		if (!SIGPIPE_MASKED(conn)) \
		{ \
			if (pq_block_sigpipe(&(spinfo).oldsigmask, \
								 &(spinfo).sigpipe_pending) < 0) \
				failaction; \
		} \
	} while (0)

#define REMEMBER_EPIPE(spinfo, cond) \
	do { \
		if (cond) \
			(spinfo).got_epipe = true; \
	} while (0)

#define RESTORE_SIGPIPE(conn, spinfo) \
	do { \
		if (!SIGPIPE_MASKED(conn)) \
			pq_reset_sigpipe(&(spinfo).oldsigmask, (spinfo).sigpipe_pending, \
							 (spinfo).got_epipe); \
	} while (0)
#else							/* !ENABLE_THREAD_SAFETY */

#define DECLARE_SIGPIPE_INFO(spinfo) pqsigfunc spinfo = NULL

#define DISABLE_SIGPIPE(conn, spinfo, failaction) \
	do { \
		if (!SIGPIPE_MASKED(conn)) \
			spinfo = pqsignal(SIGPIPE, SIG_IGN); \
	} while (0)

#define REMEMBER_EPIPE(spinfo, cond)

#define RESTORE_SIGPIPE(conn, spinfo) \
	do { \
		if (!SIGPIPE_MASKED(conn)) \
			pqsignal(SIGPIPE, spinfo); \
	} while (0)
#endif   /* ENABLE_THREAD_SAFETY */
#else							/* WIN32 */

#define DECLARE_SIGPIPE_INFO(spinfo)
#define DISABLE_SIGPIPE(conn, spinfo, failaction)
#define REMEMBER_EPIPE(spinfo, cond)
#define RESTORE_SIGPIPE(conn, spinfo)
#endif   /* WIN32 */

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
	PQinitOpenSSL(do_init, do_init);
}

/*
 *	Exported function to allow application to tell us it's already
 *	initialized OpenSSL and/or libcrypto.
 */
void
PQinitOpenSSL(int do_ssl, int do_crypto)
{
#ifdef USE_SSL
#ifdef ENABLE_THREAD_SAFETY

	/*
	 * Disallow changing the flags while we have open connections, else we'd
	 * get completely confused.
	 */
	if (ssl_open_connections != 0)
		return;
#endif

	pq_init_ssl_lib = do_ssl;
	pq_init_crypto_lib = do_crypto;
#endif
}

/*
 *	Initialize global SSL context
 */
int
pqsecure_initialize(PGconn *conn)
{
	int			r = 0;

#ifdef USE_SSL
	r = init_ssl_system(conn);
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
	destroySSL();
#endif
}

/*
 *	Begin or continue negotiating a secure session.
 */
PostgresPollingStatusType
pqsecure_open_client(PGconn *conn)
{
#ifdef USE_SSL
	/* First time through? */
	if (conn->ssl == NULL)
	{
		/* We cannot use MSG_NOSIGNAL to block SIGPIPE when using SSL */
		conn->sigpipe_flag = false;

		/* Create a connection-specific SSL object */
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
		 * Load client certificate, private key, and trusted CA certs.
		 */
		if (initialize_SSL(conn) != 0)
		{
			/* initialize_SSL already put a message in conn->errorMessage */
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
 *
 * On failure, this function is responsible for putting a suitable message
 * into conn->errorMessage.  The caller must still inspect errno, but only
 * to determine whether to continue/retry after error.
 */
ssize_t
pqsecure_read(PGconn *conn, void *ptr, size_t len)
{
	ssize_t		n;
	int			result_errno = 0;
	char		sebuf[256];

#ifdef USE_SSL
	if (conn->ssl)
	{
		int			err;

		DECLARE_SIGPIPE_INFO(spinfo);

		/* SSL_read can write to the socket, so we need to disable SIGPIPE */
		DISABLE_SIGPIPE(conn, spinfo, return -1);

rloop:
		SOCK_ERRNO_SET(0);
		n = SSL_read(conn->ssl, ptr, len);
		err = SSL_get_error(conn->ssl, n);
		switch (err)
		{
			case SSL_ERROR_NONE:
				if (n < 0)
				{
					/* Not supposed to happen, so we don't translate the msg */
					printfPQExpBuffer(&conn->errorMessage,
									  "SSL_read failed but did not provide error information\n");
					/* assume the connection is broken */
					result_errno = ECONNRESET;
				}
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
				if (n < 0)
				{
					result_errno = SOCK_ERRNO;
					REMEMBER_EPIPE(spinfo, result_errno == EPIPE);
					if (result_errno == EPIPE ||
						result_errno == ECONNRESET)
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext(
											  "server closed the connection unexpectedly\n"
											  "\tThis probably means the server terminated abnormally\n"
											  "\tbefore or while processing the request.\n"));
					else
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext("SSL SYSCALL error: %s\n"),
										  SOCK_STRERROR(result_errno,
														sebuf, sizeof(sebuf)));
				}
				else
				{
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("SSL SYSCALL error: EOF detected\n"));
					/* assume the connection is broken */
					result_errno = ECONNRESET;
					n = -1;
				}
				break;
			case SSL_ERROR_SSL:
				{
					char	   *errm = SSLerrmessage();

					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("SSL error: %s\n"), errm);
					SSLerrfree(errm);
					/* assume the connection is broken */
					result_errno = ECONNRESET;
					n = -1;
					break;
				}
			case SSL_ERROR_ZERO_RETURN:
				/*
				 * Per OpenSSL documentation, this error code is only returned
				 * for a clean connection closure, so we should not report it
				 * as a server crash.
				 */
				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("SSL connection has been closed unexpectedly\n"));
				result_errno = ECONNRESET;
				n = -1;
				break;
			default:
				printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("unrecognized SSL error code: %d\n"),
								  err);
				/* assume the connection is broken */
				result_errno = ECONNRESET;
				n = -1;
				break;
		}

		RESTORE_SIGPIPE(conn, spinfo);
	}
	else
#endif /* USE_SSL */
	{
		n = recv(conn->sock, ptr, len, 0);

		if (n < 0)
		{
			result_errno = SOCK_ERRNO;

			/* Set error message if appropriate */
			switch (result_errno)
			{
#ifdef EAGAIN
				case EAGAIN:
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
				case EWOULDBLOCK:
#endif
				case EINTR:
					/* no error message, caller is expected to retry */
					break;

#ifdef ECONNRESET
				case ECONNRESET:
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext(
										  "server closed the connection unexpectedly\n"
										  "\tThis probably means the server terminated abnormally\n"
										  "\tbefore or while processing the request.\n"));
					break;
#endif

				default:
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("could not receive data from server: %s\n"),
									  SOCK_STRERROR(result_errno,
													sebuf, sizeof(sebuf)));
					break;
			}
		}
	}

	/* ensure we return the intended errno to caller */
	SOCK_ERRNO_SET(result_errno);

	return n;
}

/*
 *	Write data to a secure connection.
 *
 * On failure, this function is responsible for putting a suitable message
 * into conn->errorMessage.  The caller must still inspect errno, but only
 * to determine whether to continue/retry after error.
 */
ssize_t
pqsecure_write(PGconn *conn, const void *ptr, size_t len)
{
	ssize_t		n;
	int			result_errno = 0;
	char		sebuf[256];

	DECLARE_SIGPIPE_INFO(spinfo);

#ifdef USE_SSL
	if (conn->ssl)
	{
		int			err;

		DISABLE_SIGPIPE(conn, spinfo, return -1);

		SOCK_ERRNO_SET(0);
		n = SSL_write(conn->ssl, ptr, len);
		err = SSL_get_error(conn->ssl, n);
		switch (err)
		{
			case SSL_ERROR_NONE:
				if (n < 0)
				{
					/* Not supposed to happen, so we don't translate the msg */
					printfPQExpBuffer(&conn->errorMessage,
									  "SSL_write failed but did not provide error information\n");
					/* assume the connection is broken */
					result_errno = ECONNRESET;
				}
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
				if (n < 0)
				{
					result_errno = SOCK_ERRNO;
					REMEMBER_EPIPE(spinfo, result_errno == EPIPE);
					if (result_errno == EPIPE ||
						result_errno == ECONNRESET)
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext(
											  "server closed the connection unexpectedly\n"
											  "\tThis probably means the server terminated abnormally\n"
											  "\tbefore or while processing the request.\n"));
					else
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext("SSL SYSCALL error: %s\n"),
										  SOCK_STRERROR(result_errno,
														sebuf, sizeof(sebuf)));
				}
				else
				{
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("SSL SYSCALL error: EOF detected\n"));
					/* assume the connection is broken */
					result_errno = ECONNRESET;
					n = -1;
				}
				break;
			case SSL_ERROR_SSL:
				{
					char	   *errm = SSLerrmessage();

					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("SSL error: %s\n"), errm);
					SSLerrfree(errm);
					/* assume the connection is broken */
					result_errno = ECONNRESET;
					n = -1;
					break;
				}
			case SSL_ERROR_ZERO_RETURN:
				/*
				 * Per OpenSSL documentation, this error code is only returned
				 * for a clean connection closure, so we should not report it
				 * as a server crash.
				 */
				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("SSL connection has been closed unexpectedly\n"));
				result_errno = ECONNRESET;
				n = -1;
				break;
			default:
				printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("unrecognized SSL error code: %d\n"),
								  err);
				/* assume the connection is broken */
				result_errno = ECONNRESET;
				n = -1;
				break;
		}
	}
	else
#endif /* USE_SSL */
	{
		int			flags = 0;

#ifdef MSG_NOSIGNAL
		if (conn->sigpipe_flag)
			flags |= MSG_NOSIGNAL;

retry_masked:
#endif   /* MSG_NOSIGNAL */

		DISABLE_SIGPIPE(conn, spinfo, return -1);

		n = send(conn->sock, ptr, len, flags);

		if (n < 0)
		{
			result_errno = SOCK_ERRNO;

			/*
			 * If we see an EINVAL, it may be because MSG_NOSIGNAL isn't
			 * available on this machine.  So, clear sigpipe_flag so we don't
			 * try the flag again, and retry the send().
			 */
#ifdef MSG_NOSIGNAL
			if (flags != 0 && result_errno == EINVAL)
			{
				conn->sigpipe_flag = false;
				flags = 0;
				goto retry_masked;
			}
#endif   /* MSG_NOSIGNAL */

			/* Set error message if appropriate */
			switch (result_errno)
			{
#ifdef EAGAIN
				case EAGAIN:
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
				case EWOULDBLOCK:
#endif
				case EINTR:
					/* no error message, caller is expected to retry */
					break;

				case EPIPE:
					/* Set flag for EPIPE */
					REMEMBER_EPIPE(spinfo, true);
					/* FALL THRU */

#ifdef ECONNRESET
				case ECONNRESET:
#endif
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext(
										  "server closed the connection unexpectedly\n"
										  "\tThis probably means the server terminated abnormally\n"
										  "\tbefore or while processing the request.\n"));
					break;

				default:
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("could not send data to server: %s\n"),
									  SOCK_STRERROR(result_errno,
													sebuf, sizeof(sebuf)));
					break;
			}
		}
	}

	RESTORE_SIGPIPE(conn, spinfo);

	/* ensure we return the intended errno to caller */
	SOCK_ERRNO_SET(result_errno);

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


/*
 * Check if a wildcard certificate matches the server hostname.
 *
 * The rule for this is:
 *	1. We only match the '*' character as wildcard
 *	2. We match only wildcards at the start of the string
 *	3. The '*' character does *not* match '.', meaning that we match only
 *	   a single pathname component.
 *	4. We don't support more than one '*' in a single pattern.
 *
 * This is roughly in line with RFC2818, but contrary to what most browsers
 * appear to be implementing (point 3 being the difference)
 *
 * Matching is always case-insensitive, since DNS is case insensitive.
 */
static int
wildcard_certificate_match(const char *pattern, const char *string)
{
	int			lenpat = strlen(pattern);
	int			lenstr = strlen(string);

	/* If we don't start with a wildcard, it's not a match (rule 1 & 2) */
	if (lenpat < 3 ||
		pattern[0] != '*' ||
		pattern[1] != '.')
		return 0;

	if (lenpat > lenstr)
		/* If pattern is longer than the string, we can never match */
		return 0;

	if (pg_strcasecmp(pattern + 1, string + lenstr - lenpat + 1) != 0)

		/*
		 * If string does not end in pattern (minus the wildcard), we don't
		 * match
		 */
		return 0;

	if (strchr(string, '.') < string + lenstr - lenpat)

		/*
		 * If there is a dot left of where the pattern started to match, we
		 * don't match (rule 3)
		 */
		return 0;

	/* String ended with pattern, and didn't have a dot before, so we match */
	return 1;
}


/*
 *	Verify that common name resolves to peer.
 */
static bool
verify_peer_name_matches_certificate(PGconn *conn)
{
	/*
	 * If told not to verify the peer name, don't do it. Return true
	 * indicating that the verification was successful.
	 */
	if (strcmp(conn->sslmode, "verify-full") != 0)
		return true;

	if (!(conn->pghost && conn->pghost[0] != '\0'))
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("host name must be specified for a verified SSL connection\n"));
		return false;
	}
	else
	{
		/*
		 * Compare CN to originally given hostname.
		 *
		 * XXX: Should support alternate names here
		 */
		if (pg_strcasecmp(conn->peer_cn, conn->pghost) == 0)
			/* Exact name match */
			return true;
		else if (wildcard_certificate_match(conn->peer_cn, conn->pghost))
			/* Matched wildcard certificate */
			return true;
		else
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("server common name \"%s\" does not match host name \"%s\"\n"),
							  conn->peer_cn, conn->pghost);
			return false;
		}
	}
}

#ifdef ENABLE_THREAD_SAFETY
/*
 *	Callback functions for OpenSSL internal locking
 */

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
	{
		if (pthread_mutex_lock(&pq_lockarray[n]))
			PGTHREAD_ERROR("failed to lock mutex");
	}
	else
	{
		if (pthread_mutex_unlock(&pq_lockarray[n]))
			PGTHREAD_ERROR("failed to unlock mutex");
	}
}
#endif   /* ENABLE_THREAD_SAFETY */

/*
 * Initialize SSL system, in particular creating the SSL_context object
 * that will be shared by all SSL-using connections in this process.
 *
 * In threadsafe mode, this includes setting up libcrypto callback functions
 * to do thread locking.
 *
 * If the caller has told us (through PQinitOpenSSL) that he's taking care
 * of libcrypto, we expect that callbacks are already set, and won't try to
 * override it.
 *
 * The conn parameter is only used to be able to pass back an error
 * message - no connection-local setup is made here.
 *
 * Returns 0 if OK, -1 on failure (with a message in conn->errorMessage).
 */
static int
init_ssl_system(PGconn *conn)
{
#ifdef ENABLE_THREAD_SAFETY
#ifdef WIN32
	/* Also see similar code in fe-connect.c, default_threadlock() */
	if (ssl_config_mutex == NULL)
	{
		while (InterlockedExchange(&win32_ssl_create_mutex, 1) == 1)
			 /* loop, another thread own the lock */ ;
		if (ssl_config_mutex == NULL)
		{
			if (pthread_mutex_init(&ssl_config_mutex, NULL))
				return -1;
		}
		InterlockedExchange(&win32_ssl_create_mutex, 0);
	}
#endif
	if (pthread_mutex_lock(&ssl_config_mutex))
		return -1;

	if (pq_init_crypto_lib)
	{
		/*
		 * If necessary, set up an array to hold locks for libcrypto.
		 * libcrypto will tell us how big to make this array.
		 */
		if (pq_lockarray == NULL)
		{
			int			i;

			pq_lockarray = malloc(sizeof(pthread_mutex_t) * CRYPTO_num_locks());
			if (!pq_lockarray)
			{
				pthread_mutex_unlock(&ssl_config_mutex);
				return -1;
			}
			for (i = 0; i < CRYPTO_num_locks(); i++)
			{
				if (pthread_mutex_init(&pq_lockarray[i], NULL))
				{
					free(pq_lockarray);
					pq_lockarray = NULL;
					pthread_mutex_unlock(&ssl_config_mutex);
					return -1;
				}
			}
		}

		if (ssl_open_connections++ == 0)
		{
			/* These are only required for threaded libcrypto applications */
			CRYPTO_set_id_callback(pq_threadidcallback);
			CRYPTO_set_locking_callback(pq_lockingcallback);
		}
	}
#endif   /* ENABLE_THREAD_SAFETY */

	if (!SSL_context)
	{
		if (pq_init_ssl_lib)
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
			pthread_mutex_unlock(&ssl_config_mutex);
#endif
			return -1;
		}

		/*
		 * Disable OpenSSL's moving-write-buffer sanity check, because it
		 * causes unnecessary failures in nonblocking send cases.
		 */
		SSL_CTX_set_mode(SSL_context, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	}

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&ssl_config_mutex);
#endif
	return 0;
}

/*
 *	This function is needed because if the libpq library is unloaded
 *	from the application, the callback functions will no longer exist when
 *	libcrypto is used by other parts of the system.  For this reason,
 *	we unregister the callback functions when the last libpq
 *	connection is closed.  (The same would apply for OpenSSL callbacks
 *	if we had any.)
 *
 *	Callbacks are only set when we're compiled in threadsafe mode, so
 *	we only need to remove them in this case.
 */
static void
destroy_ssl_system(void)
{
#ifdef ENABLE_THREAD_SAFETY
	/* Mutex is created in initialize_ssl_system() */
	if (pthread_mutex_lock(&ssl_config_mutex))
		return;

	if (pq_init_crypto_lib && ssl_open_connections > 0)
		--ssl_open_connections;

	if (pq_init_crypto_lib && ssl_open_connections == 0)
	{
		/* No connections left, unregister libcrypto callbacks */
		CRYPTO_set_locking_callback(NULL);
		CRYPTO_set_id_callback(NULL);

		/*
		 * We don't free the lock array. If we get another connection in this
		 * process, we will just re-use it with the existing mutexes.
		 *
		 * This means we leak a little memory on repeated load/unload of the
		 * library.
		 */
	}

	pthread_mutex_unlock(&ssl_config_mutex);
#endif
}

/*
 *	Initialize (potentially) per-connection SSL data, namely the
 *	client certificate, private key, and trusted CA certs.
 *
 *	conn->ssl must already be created.	It receives the connection's client
 *	certificate and private key.  Note however that certificates also get
 *	loaded into the SSL_context object, and are therefore accessible to all
 *	connections in this process.  This should be OK as long as there aren't
 *	any hash collisions among the certs.
 *
 *	Returns 0 if OK, -1 on failure (with a message in conn->errorMessage).
 */
static int
initialize_SSL(PGconn *conn)
{
	struct stat buf;
	char		homedir[MAXPGPATH];
	char		fnbuf[MAXPGPATH];
	char		sebuf[256];
	bool		have_homedir;
	bool		have_cert;
	EVP_PKEY   *pkey = NULL;

	/*
	 * We'll need the home directory if any of the relevant parameters are
	 * defaulted.  If pqGetHomeDirectory fails, act as though none of the
	 * files could be found.
	 */
	if (!(conn->sslcert && strlen(conn->sslcert) > 0) ||
		!(conn->sslkey && strlen(conn->sslkey) > 0) ||
		!(conn->sslrootcert && strlen(conn->sslrootcert) > 0) ||
		!(conn->sslcrl && strlen(conn->sslcrl) > 0))
		have_homedir = pqGetHomeDirectory(homedir, sizeof(homedir));
	else	/* won't need it */
		have_homedir = false;

	/* Read the client certificate file */
	if (conn->sslcert && strlen(conn->sslcert) > 0)
		strncpy(fnbuf, conn->sslcert, sizeof(fnbuf));
	else if (have_homedir)
		snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, USER_CERT_FILE);
	else
		fnbuf[0] = '\0';

	if (fnbuf[0] == '\0')
	{
		/* no home directory, proceed without a client cert */
		have_cert = false;
	}
	else if (stat(fnbuf, &buf) != 0)
	{
		/*
		 * If file is not present, just go on without a client cert; server
		 * might or might not accept the connection.  Any other error,
		 * however, is grounds for complaint.
		 */
		if (errno != ENOENT)
		{
			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not open certificate file \"%s\": %s\n"),
							  fnbuf, pqStrerror(errno, sebuf, sizeof(sebuf)));
			return -1;
		}
		have_cert = false;
	}
	else
	{
		/*
		 * Cert file exists, so load it.  Since OpenSSL doesn't provide the
		 * equivalent of "SSL_use_certificate_chain_file", we actually have to
		 * load the file twice.  The first call loads any extra certs after
		 * the first one into chain-cert storage associated with the
		 * SSL_context.  The second call loads the first cert (only) into the
		 * SSL object, where it will be correctly paired with the private key
		 * we load below.  We do it this way so that each connection
		 * understands which subject cert to present, in case different
		 * sslcert settings are used for different connections in the same
		 * process.
		 */
		if (SSL_CTX_use_certificate_chain_file(SSL_context, fnbuf) != 1)
		{
			char	   *err = SSLerrmessage();

			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not read certificate file \"%s\": %s\n"),
							  fnbuf, err);
			SSLerrfree(err);
			return -1;
		}
		if (SSL_use_certificate_file(conn->ssl, fnbuf, SSL_FILETYPE_PEM) != 1)
		{
			char	   *err = SSLerrmessage();

			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not read certificate file \"%s\": %s\n"),
							  fnbuf, err);
			SSLerrfree(err);
			return -1;
		}
		/* need to load the associated private key, too */
		have_cert = true;
	}

	/*
	 * Read the SSL key. If a key is specified, treat it as an engine:key
	 * combination if there is colon present - we don't support files with
	 * colon in the name. The exception is if the second character is a colon,
	 * in which case it can be a Windows filename with drive specification.
	 */
	if (have_cert && conn->sslkey && strlen(conn->sslkey) > 0)
	{
#ifdef USE_SSL_ENGINE
		if (strchr(conn->sslkey, ':')
#ifdef WIN32
			&& conn->sslkey[1] != ':'
#endif
			)
		{
			/* Colon, but not in second character, treat as engine:key */
			char	   *engine_str = strdup(conn->sslkey);
			char	   *engine_colon = strchr(engine_str, ':');

			*engine_colon = '\0';		/* engine_str now has engine name */
			engine_colon++;		/* engine_colon now has key name */

			conn->engine = ENGINE_by_id(engine_str);
			if (conn->engine == NULL)
			{
				char	   *err = SSLerrmessage();

				printfPQExpBuffer(&conn->errorMessage,
					 libpq_gettext("could not load SSL engine \"%s\": %s\n"),
								  engine_str, err);
				SSLerrfree(err);
				free(engine_str);
				return -1;
			}

			if (ENGINE_init(conn->engine) == 0)
			{
				char	   *err = SSLerrmessage();

				printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("could not initialize SSL engine \"%s\": %s\n"),
								  engine_str, err);
				SSLerrfree(err);
				ENGINE_free(conn->engine);
				conn->engine = NULL;
				free(engine_str);
				return -1;
			}

			pkey = ENGINE_load_private_key(conn->engine, engine_colon,
										   NULL, NULL);
			if (pkey == NULL)
			{
				char	   *err = SSLerrmessage();

				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("could not read private SSL key \"%s\" from engine \"%s\": %s\n"),
								  engine_colon, engine_str, err);
				SSLerrfree(err);
				ENGINE_finish(conn->engine);
				ENGINE_free(conn->engine);
				conn->engine = NULL;
				free(engine_str);
				return -1;
			}
			if (SSL_use_PrivateKey(conn->ssl, pkey) != 1)
			{
				char	   *err = SSLerrmessage();

				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("could not load private SSL key \"%s\" from engine \"%s\": %s\n"),
								  engine_colon, engine_str, err);
				SSLerrfree(err);
				ENGINE_finish(conn->engine);
				ENGINE_free(conn->engine);
				conn->engine = NULL;
				free(engine_str);
				return -1;
			}

			free(engine_str);

			fnbuf[0] = '\0';	/* indicate we're not going to load from a
								 * file */
		}
		else
#endif   /* USE_SSL_ENGINE */
		{
			/* PGSSLKEY is not an engine, treat it as a filename */
			strncpy(fnbuf, conn->sslkey, sizeof(fnbuf));
		}
	}
	else if (have_homedir)
	{
		/* No PGSSLKEY specified, load default file */
		snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, USER_KEY_FILE);
	}
	else
		fnbuf[0] = '\0';

	if (have_cert && fnbuf[0] != '\0')
	{
		/* read the client key from file */

		if (stat(fnbuf, &buf) != 0)
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("certificate present, but not private key file \"%s\"\n"),
							  fnbuf);
			return -1;
		}
#ifndef WIN32
		if (!S_ISREG(buf.st_mode) || buf.st_mode & (S_IRWXG | S_IRWXO))
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("private key file \"%s\" has group or world access; permissions should be u=rw (0600) or less\n"),
							  fnbuf);
			return -1;
		}
#endif

		if (SSL_use_PrivateKey_file(conn->ssl, fnbuf, SSL_FILETYPE_PEM) != 1)
		{
			char	   *err = SSLerrmessage();

			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not load private key file \"%s\": %s\n"),
							  fnbuf, err);
			SSLerrfree(err);
			return -1;
		}
	}

	/* verify that the cert and key go together */
	if (have_cert &&
		SSL_check_private_key(conn->ssl) != 1)
	{
		char	   *err = SSLerrmessage();

		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("certificate does not match private key file \"%s\": %s\n"),
						  fnbuf, err);
		SSLerrfree(err);
		return -1;
	}

	/*
	 * If the root cert file exists, load it so we can perform certificate
	 * verification. If sslmode is "verify-full" we will also do further
	 * verification after the connection has been completed.
	 */
	if (conn->sslrootcert && strlen(conn->sslrootcert) > 0)
		strncpy(fnbuf, conn->sslrootcert, sizeof(fnbuf));
	else if (have_homedir)
		snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, ROOT_CERT_FILE);
	else
		fnbuf[0] = '\0';

	if (fnbuf[0] != '\0' &&
		stat(fnbuf, &buf) == 0)
	{
		X509_STORE *cvstore;

		if (SSL_CTX_load_verify_locations(SSL_context, fnbuf, NULL) != 1)
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
			if (conn->sslcrl && strlen(conn->sslcrl) > 0)
				strncpy(fnbuf, conn->sslcrl, sizeof(fnbuf));
			else if (have_homedir)
				snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, ROOT_CRL_FILE);
			else
				fnbuf[0] = '\0';

			/* Set the flags to check against the complete CRL chain */
			if (fnbuf[0] != '\0' &&
				X509_STORE_load_locations(cvstore, fnbuf, NULL) == 1)
			{
				/* OpenSSL 0.96 does not support X509_V_FLAG_CRL_CHECK */
#ifdef X509_V_FLAG_CRL_CHECK
				X509_STORE_set_flags(cvstore,
						  X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
#else
				char	   *err = SSLerrmessage();

				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("SSL library does not support CRL certificates (file \"%s\")\n"),
								  fnbuf);
				SSLerrfree(err);
				return -1;
#endif
			}
			/* if not found, silently ignore;  we do not require CRL */
		}

		SSL_set_verify(conn->ssl, SSL_VERIFY_PEER, verify_cb);
	}
	else
	{
		/*
		 * stat() failed; assume root file doesn't exist.  If sslmode is
		 * verify-ca or verify-full, this is an error.	Otherwise, continue
		 * without performing any server cert verification.
		 */
		if (conn->sslmode[0] == 'v')	/* "verify-ca" or "verify-full" */
		{
			/*
			 * The only way to reach here with an empty filename is if
			 * pqGetHomeDirectory failed.  That's a sufficiently unusual case
			 * that it seems worth having a specialized error message for it.
			 */
			if (fnbuf[0] == '\0')
				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("could not get home directory to locate root certificate file\n"
												"Either provide the file or change sslmode to disable server certificate verification.\n"));
			else
				printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("root certificate file \"%s\" does not exist\n"
							  "Either provide the file or change sslmode to disable server certificate verification.\n"), fnbuf);
			return -1;
		}
	}

	return 0;
}

static void
destroySSL(void)
{
	destroy_ssl_system();
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
					char	   *err = SSLerrmessage();

					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("SSL error: %s\n"),
									  err);
					SSLerrfree(err);
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

	/*
	 * We already checked the server certificate in initialize_SSL() using
	 * SSL_CTX_set_verify(), if root.crt exists.
	 */

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

	r = X509_NAME_get_text_by_NID(X509_get_subject_name(conn->peer),
								  NID_commonName, conn->peer_cn, SM_USER);
	conn->peer_cn[SM_USER] = '\0';		/* buffer is SM_USER+1 chars! */
	if (r == -1)
	{
		/* Unable to get the CN, set it to blank so it can't be used */
		conn->peer_cn[0] = '\0';
	}
	else
	{
		/*
		 * Reject embedded NULLs in certificate common name to prevent attacks
		 * like CVE-2009-4034.
		 */
		if (r != strlen(conn->peer_cn))
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("SSL certificate's common name contains embedded null\n"));
			close_SSL(conn);
			return PGRES_POLLING_FAILED;
		}
	}

	if (!verify_peer_name_matches_certificate(conn))
	{
		close_SSL(conn);
		return PGRES_POLLING_FAILED;
	}

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
		DECLARE_SIGPIPE_INFO(spinfo);

		DISABLE_SIGPIPE(conn, spinfo, (void) 0);
		SSL_shutdown(conn->ssl);
		SSL_free(conn->ssl);
		conn->ssl = NULL;
		pqsecure_destroy();
		/* We have to assume we got EPIPE */
		REMEMBER_EPIPE(spinfo, true);
		RESTORE_SIGPIPE(conn, spinfo);
	}

	if (conn->peer)
	{
		X509_free(conn->peer);
		conn->peer = NULL;
	}

#ifdef USE_SSL_ENGINE
	if (conn->engine)
	{
		ENGINE_finish(conn->engine);
		ENGINE_free(conn->engine);
		conn->engine = NULL;
	}
#endif
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
