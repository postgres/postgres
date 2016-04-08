/*-------------------------------------------------------------------------
 *
 * fe-secure-openssl.c
 *	  OpenSSL support
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-secure-openssl.c
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

#include <openssl/ssl.h>
#if (SSLEAY_VERSION_NUMBER >= 0x00907000L)
#include <openssl/conf.h>
#endif
#ifdef USE_SSL_ENGINE
#include <openssl/engine.h>
#endif
#include <openssl/x509v3.h>

static bool verify_peer_name_matches_certificate(PGconn *);
static int	verify_cb(int ok, X509_STORE_CTX *ctx);
static int verify_peer_name_matches_certificate_name(PGconn *conn,
										  ASN1_STRING *name,
										  char **store_name);
static void destroy_ssl_system(void);
static int	initialize_SSL(PGconn *conn);
static PostgresPollingStatusType open_client_SSL(PGconn *);
static char *SSLerrmessage(unsigned long ecode);
static void SSLerrfree(char *buf);

static int	my_sock_read(BIO *h, char *buf, int size);
static int	my_sock_write(BIO *h, const char *buf, int size);
static BIO_METHOD *my_BIO_s_socket(void);
static int	my_SSL_set_fd(PGconn *conn, int fd);


static bool pq_init_ssl_lib = true;
static bool pq_init_crypto_lib = true;

/*
 * SSL_context is currently shared between threads and therefore we need to be
 * careful to lock around any usage of it when providing thread safety.
 * ssl_config_mutex is the mutex that we use to protect it.
 */
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


/* ------------------------------------------------------------ */
/*			 Procedures common to all secure sessions			*/
/* ------------------------------------------------------------ */

/*
 *	Exported function to allow application to tell us it's already
 *	initialized OpenSSL and/or libcrypto.
 */
void
pgtls_init_library(bool do_ssl, int do_crypto)
{
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
}

/*
 *	Begin or continue negotiating a secure session.
 */
PostgresPollingStatusType
pgtls_open_client(PGconn *conn)
{
	/* First time through? */
	if (conn->ssl == NULL)
	{
#ifdef ENABLE_THREAD_SAFETY
		int			rc;
#endif

#ifdef ENABLE_THREAD_SAFETY
		if ((rc = pthread_mutex_lock(&ssl_config_mutex)))
		{
			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not acquire mutex: %s\n"), strerror(rc));
			return PGRES_POLLING_FAILED;
		}
#endif
		/* Create a connection-specific SSL object */
		if (!(conn->ssl = SSL_new(SSL_context)) ||
			!SSL_set_app_data(conn->ssl, conn) ||
			!my_SSL_set_fd(conn, conn->sock))
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			printfPQExpBuffer(&conn->errorMessage,
				   libpq_gettext("could not establish SSL connection: %s\n"),
							  err);
			SSLerrfree(err);
#ifdef ENABLE_THREAD_SAFETY
			pthread_mutex_unlock(&ssl_config_mutex);
#endif
			pgtls_close(conn);

			return PGRES_POLLING_FAILED;
		}
		conn->ssl_in_use = true;

#ifdef ENABLE_THREAD_SAFETY
		pthread_mutex_unlock(&ssl_config_mutex);
#endif

		/*
		 * Load client certificate, private key, and trusted CA certs.
		 */
		if (initialize_SSL(conn) != 0)
		{
			/* initialize_SSL already put a message in conn->errorMessage */
			pgtls_close(conn);
			return PGRES_POLLING_FAILED;
		}
	}

	/* Begin or continue the actual handshake */
	return open_client_SSL(conn);
}

/*
 *	Is there unread data waiting in the SSL read buffer?
 */
bool
pgtls_read_pending(PGconn *conn)
{
	return SSL_pending(conn->ssl);
}

/*
 *	Read data from a secure connection.
 *
 * On failure, this function is responsible for putting a suitable message
 * into conn->errorMessage.  The caller must still inspect errno, but only
 * to determine whether to continue/retry after error.
 */
ssize_t
pgtls_read(PGconn *conn, void *ptr, size_t len)
{
	ssize_t		n;
	int			result_errno = 0;
	char		sebuf[256];
	int			err;
	unsigned long ecode;

rloop:
	/*
	 * Prepare to call SSL_get_error() by clearing thread's OpenSSL error
	 * queue.  In general, the current thread's error queue must be empty
	 * before the TLS/SSL I/O operation is attempted, or SSL_get_error()
	 * will not work reliably.  Since the possibility exists that other
	 * OpenSSL clients running in the same thread but not under our control
	 * will fail to call ERR_get_error() themselves (after their own I/O
	 * operations), pro-actively clear the per-thread error queue now.
	 */
	SOCK_ERRNO_SET(0);
	ERR_clear_error();
	n = SSL_read(conn->ssl, ptr, len);
	err = SSL_get_error(conn->ssl, n);

	/*
	 * Other clients of OpenSSL may fail to call ERR_get_error(), but we
	 * always do, so as to not cause problems for OpenSSL clients that
	 * don't call ERR_clear_error() defensively.  Be sure that this
	 * happens by calling now.  SSL_get_error() relies on the OpenSSL
	 * per-thread error queue being intact, so this is the earliest
	 * possible point ERR_get_error() may be called.
	 */
	ecode = (err != SSL_ERROR_NONE || n < 0) ? ERR_get_error() : 0;
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
				char	   *errm = SSLerrmessage(ecode);

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
			 * Per OpenSSL documentation, this error code is only returned for
			 * a clean connection closure, so we should not report it as a
			 * server crash.
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
pgtls_write(PGconn *conn, const void *ptr, size_t len)
{
	ssize_t		n;
	int			result_errno = 0;
	char		sebuf[256];
	int			err;
	unsigned long ecode;

	SOCK_ERRNO_SET(0);
	ERR_clear_error();
	n = SSL_write(conn->ssl, ptr, len);
	err = SSL_get_error(conn->ssl, n);
	ecode = (err != SSL_ERROR_NONE || n < 0) ? ERR_get_error() : 0;
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
			 * Returning 0 here causes caller to wait for write-ready, which
			 * is not really the right thing, but it's the best we can do.
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
				if (result_errno == EPIPE || result_errno == ECONNRESET)
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
				char	   *errm = SSLerrmessage(ecode);

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
			 * Per OpenSSL documentation, this error code is only returned for
			 * a clean connection closure, so we should not report it as a
			 * server crash.
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

	/* ensure we return the intended errno to caller */
	SOCK_ERRNO_SET(result_errno);

	return n;
}

/* ------------------------------------------------------------ */
/*						OpenSSL specific code					*/
/* ------------------------------------------------------------ */

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
 * Check if a name from a server's certificate matches the peer's hostname.
 *
 * Returns 1 if the name matches, and 0 if it does not. On error, returns
 * -1, and sets the libpq error message.
 *
 * The name extracted from the certificate is returned in *store_name. The
 * caller is responsible for freeing it.
 */
static int
verify_peer_name_matches_certificate_name(PGconn *conn, ASN1_STRING *name_entry,
										  char **store_name)
{
	int			len;
	char	   *name;
	unsigned char *namedata;
	int			result;

	*store_name = NULL;

	/* Should not happen... */
	if (name_entry == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
				 libpq_gettext("SSL certificate's name entry is missing\n"));
		return -1;
	}

	/*
	 * GEN_DNS can be only IA5String, equivalent to US ASCII.
	 *
	 * There is no guarantee the string returned from the certificate is
	 * NULL-terminated, so make a copy that is.
	 */
	namedata = ASN1_STRING_data(name_entry);
	len = ASN1_STRING_length(name_entry);
	name = malloc(len + 1);
	if (name == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("out of memory\n"));
		return -1;
	}
	memcpy(name, namedata, len);
	name[len] = '\0';

	/*
	 * Reject embedded NULLs in certificate common or alternative name to
	 * prevent attacks like CVE-2009-4034.
	 */
	if (len != strlen(name))
	{
		free(name);
		printfPQExpBuffer(&conn->errorMessage,
		   libpq_gettext("SSL certificate's name contains embedded null\n"));
		return -1;
	}

	if (pg_strcasecmp(name, conn->pghost) == 0)
	{
		/* Exact name match */
		result = 1;
	}
	else if (wildcard_certificate_match(name, conn->pghost))
	{
		/* Matched wildcard name */
		result = 1;
	}
	else
	{
		result = 0;
	}

	*store_name = name;
	return result;
}

/*
 *	Verify that the server certificate matches the hostname we connected to.
 *
 * The certificate's Common Name and Subject Alternative Names are considered.
 */
static bool
verify_peer_name_matches_certificate(PGconn *conn)
{
	int			names_examined = 0;
	bool		found_match = false;
	bool		got_error = false;
	char	   *first_name = NULL;

	STACK_OF(GENERAL_NAME) *peer_san;
	int			i;
	int			rc;

	/*
	 * If told not to verify the peer name, don't do it. Return true
	 * indicating that the verification was successful.
	 */
	if (strcmp(conn->sslmode, "verify-full") != 0)
		return true;

	/* Check that we have a hostname to compare with. */
	if (!(conn->pghost && conn->pghost[0] != '\0'))
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("host name must be specified for a verified SSL connection\n"));
		return false;
	}

	/*
	 * First, get the Subject Alternative Names (SANs) from the certificate,
	 * and compare them against the originally given hostname.
	 */
	peer_san = (STACK_OF(GENERAL_NAME) *)
		X509_get_ext_d2i(conn->peer, NID_subject_alt_name, NULL, NULL);

	if (peer_san)
	{
		int			san_len = sk_GENERAL_NAME_num(peer_san);

		for (i = 0; i < san_len; i++)
		{
			const GENERAL_NAME *name = sk_GENERAL_NAME_value(peer_san, i);

			if (name->type == GEN_DNS)
			{
				char	   *alt_name;

				names_examined++;
				rc = verify_peer_name_matches_certificate_name(conn,
															 name->d.dNSName,
															   &alt_name);
				if (rc == -1)
					got_error = true;
				if (rc == 1)
					found_match = true;

				if (alt_name)
				{
					if (!first_name)
						first_name = alt_name;
					else
						free(alt_name);
				}
			}
			if (found_match || got_error)
				break;
		}
		sk_GENERAL_NAME_free(peer_san);
	}

	/*
	 * If there is no subjectAltName extension of type dNSName, check the
	 * Common Name.
	 *
	 * (Per RFC 2818 and RFC 6125, if the subjectAltName extension of type
	 * dNSName is present, the CN must be ignored.)
	 */
	if (names_examined == 0)
	{
		X509_NAME  *subject_name;

		subject_name = X509_get_subject_name(conn->peer);
		if (subject_name != NULL)
		{
			int			cn_index;

			cn_index = X509_NAME_get_index_by_NID(subject_name,
												  NID_commonName, -1);
			if (cn_index >= 0)
			{
				names_examined++;
				rc = verify_peer_name_matches_certificate_name(
															   conn,
													X509_NAME_ENTRY_get_data(
								X509_NAME_get_entry(subject_name, cn_index)),
															   &first_name);

				if (rc == -1)
					got_error = true;
				else if (rc == 1)
					found_match = true;
			}
		}
	}

	if (!found_match && !got_error)
	{
		/*
		 * No match. Include the name from the server certificate in the error
		 * message, to aid debugging broken configurations. If there are
		 * multiple names, only print the first one to avoid an overly long
		 * error message.
		 */
		if (names_examined > 1)
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_ngettext("server certificate for \"%s\" (and %d other name) does not match host name \"%s\"\n",
											 "server certificate for \"%s\" (and %d other names) does not match host name \"%s\"\n",
											 names_examined - 1),
							  first_name, names_examined - 1, conn->pghost);
		}
		else if (names_examined == 1)
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("server certificate for \"%s\" does not match host name \"%s\"\n"),
							  first_name, conn->pghost);
		}
		else
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("could not get server's host name from server certificate\n"));
		}
	}

	/* clean up */
	if (first_name)
		free(first_name);

	return found_match && !got_error;
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
int
pgtls_init(PGconn *conn)
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
			/*
			 * These are only required for threaded libcrypto applications,
			 * but make sure we don't stomp on them if they're already set.
			 */
			if (CRYPTO_get_id_callback() == NULL)
				CRYPTO_set_id_callback(pq_threadidcallback);
			if (CRYPTO_get_locking_callback() == NULL)
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

		/*
		 * We use SSLv23_method() because it can negotiate use of the highest
		 * mutually supported protocol version, while alternatives like
		 * TLSv1_2_method() permit only one specific version.  Note that we
		 * don't actually allow SSL v2 or v3, only TLS protocols (see below).
		 */
		SSL_context = SSL_CTX_new(SSLv23_method());
		if (!SSL_context)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			printfPQExpBuffer(&conn->errorMessage,
						 libpq_gettext("could not create SSL context: %s\n"),
							  err);
			SSLerrfree(err);
#ifdef ENABLE_THREAD_SAFETY
			pthread_mutex_unlock(&ssl_config_mutex);
#endif
			return -1;
		}

		/* Disable old protocol versions */
		SSL_CTX_set_options(SSL_context, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

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
		/*
		 * No connections left, unregister libcrypto callbacks, if no one
		 * registered different ones in the meantime.
		 */
		if (CRYPTO_get_locking_callback() == pq_lockingcallback)
			CRYPTO_set_locking_callback(NULL);
		if (CRYPTO_get_id_callback() == pq_threadidcallback)
			CRYPTO_set_id_callback(NULL);

		/*
		 * We don't free the lock array or the SSL_context. If we get another
		 * connection in this process, we will just re-use them with the
		 * existing mutexes.
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
 *	conn->ssl must already be created.  It receives the connection's client
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
		strlcpy(fnbuf, conn->sslcert, sizeof(fnbuf));
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
		if (errno != ENOENT && errno != ENOTDIR)
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
		 *
		 * NOTE: This function may also modify our SSL_context and therefore
		 * we have to lock around this call and any places where we use the
		 * SSL_context struct.
		 */
#ifdef ENABLE_THREAD_SAFETY
		int			rc;

		if ((rc = pthread_mutex_lock(&ssl_config_mutex)))
		{
			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not acquire mutex: %s\n"), strerror(rc));
			return -1;
		}
#endif
		if (SSL_CTX_use_certificate_chain_file(SSL_context, fnbuf) != 1)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not read certificate file \"%s\": %s\n"),
							  fnbuf, err);
			SSLerrfree(err);

#ifdef ENABLE_THREAD_SAFETY
			pthread_mutex_unlock(&ssl_config_mutex);
#endif
			return -1;
		}

		if (SSL_use_certificate_file(conn->ssl, fnbuf, SSL_FILETYPE_PEM) != 1)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not read certificate file \"%s\": %s\n"),
							  fnbuf, err);
			SSLerrfree(err);
#ifdef ENABLE_THREAD_SAFETY
			pthread_mutex_unlock(&ssl_config_mutex);
#endif
			return -1;
		}

		/* need to load the associated private key, too */
		have_cert = true;

#ifdef ENABLE_THREAD_SAFETY
		pthread_mutex_unlock(&ssl_config_mutex);
#endif
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
			char	   *engine_colon;

			if (engine_str == NULL)
			{
				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("out of memory\n"));
				return -1;
			}

			/* cannot return NULL because we already checked before strdup */
			engine_colon = strchr(engine_str, ':');

			*engine_colon = '\0';		/* engine_str now has engine name */
			engine_colon++;		/* engine_colon now has key name */

			conn->engine = ENGINE_by_id(engine_str);
			if (conn->engine == NULL)
			{
				char	   *err = SSLerrmessage(ERR_get_error());

				printfPQExpBuffer(&conn->errorMessage,
					 libpq_gettext("could not load SSL engine \"%s\": %s\n"),
								  engine_str, err);
				SSLerrfree(err);
				free(engine_str);
				return -1;
			}

			if (ENGINE_init(conn->engine) == 0)
			{
				char	   *err = SSLerrmessage(ERR_get_error());

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
				char	   *err = SSLerrmessage(ERR_get_error());

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
				char	   *err = SSLerrmessage(ERR_get_error());

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
			strlcpy(fnbuf, conn->sslkey, sizeof(fnbuf));
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
			char	   *err = SSLerrmessage(ERR_get_error());

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
		char	   *err = SSLerrmessage(ERR_get_error());

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
		strlcpy(fnbuf, conn->sslrootcert, sizeof(fnbuf));
	else if (have_homedir)
		snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, ROOT_CERT_FILE);
	else
		fnbuf[0] = '\0';

	if (fnbuf[0] != '\0' &&
		stat(fnbuf, &buf) == 0)
	{
		X509_STORE *cvstore;

#ifdef ENABLE_THREAD_SAFETY
		int			rc;

		if ((rc = pthread_mutex_lock(&ssl_config_mutex)))
		{
			printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("could not acquire mutex: %s\n"), strerror(rc));
			return -1;
		}
#endif
		if (SSL_CTX_load_verify_locations(SSL_context, fnbuf, NULL) != 1)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("could not read root certificate file \"%s\": %s\n"),
							  fnbuf, err);
			SSLerrfree(err);
#ifdef ENABLE_THREAD_SAFETY
			pthread_mutex_unlock(&ssl_config_mutex);
#endif
			return -1;
		}

		if ((cvstore = SSL_CTX_get_cert_store(SSL_context)) != NULL)
		{
			if (conn->sslcrl && strlen(conn->sslcrl) > 0)
				strlcpy(fnbuf, conn->sslcrl, sizeof(fnbuf));
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
				char	   *err = SSLerrmessage(ERR_get_error());

				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("SSL library does not support CRL certificates (file \"%s\")\n"),
								  fnbuf);
				SSLerrfree(err);
#ifdef ENABLE_THREAD_SAFETY
				pthread_mutex_unlock(&ssl_config_mutex);
#endif
				return -1;
#endif
			}
			/* if not found, silently ignore;  we do not require CRL */
		}
#ifdef ENABLE_THREAD_SAFETY
		pthread_mutex_unlock(&ssl_config_mutex);
#endif

		SSL_set_verify(conn->ssl, SSL_VERIFY_PEER, verify_cb);
	}
	else
	{
		/*
		 * stat() failed; assume root file doesn't exist.  If sslmode is
		 * verify-ca or verify-full, this is an error.  Otherwise, continue
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

	/*
	 * If the OpenSSL version used supports it (from 1.0.0 on) and the user
	 * requested it, disable SSL compression.
	 */
#ifdef SSL_OP_NO_COMPRESSION
	if (conn->sslcompression && conn->sslcompression[0] == '0')
	{
		SSL_set_options(conn->ssl, SSL_OP_NO_COMPRESSION);
	}
#endif

	return 0;
}

/*
 *	Attempt to negotiate SSL connection.
 */
static PostgresPollingStatusType
open_client_SSL(PGconn *conn)
{
	int			r;

	ERR_clear_error();
	r = SSL_connect(conn->ssl);
	if (r <= 0)
	{
		int			err = SSL_get_error(conn->ssl, r);
		unsigned long ecode;

		ecode = ERR_get_error();
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
					pgtls_close(conn);
					return PGRES_POLLING_FAILED;
				}
			case SSL_ERROR_SSL:
				{
					char	   *err = SSLerrmessage(ecode);

					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("SSL error: %s\n"),
									  err);
					SSLerrfree(err);
					pgtls_close(conn);
					return PGRES_POLLING_FAILED;
				}

			default:
				printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("unrecognized SSL error code: %d\n"),
								  err);
				pgtls_close(conn);
				return PGRES_POLLING_FAILED;
		}
	}

	/*
	 * We already checked the server certificate in initialize_SSL() using
	 * SSL_CTX_set_verify(), if root.crt exists.
	 */

	/* get server certificate */
	conn->peer = SSL_get_peer_certificate(conn->ssl);
	if (conn->peer == NULL)
	{
		char	   *err;

		err = SSLerrmessage(ERR_get_error());

		printfPQExpBuffer(&conn->errorMessage,
					libpq_gettext("certificate could not be obtained: %s\n"),
						  err);
		SSLerrfree(err);
		pgtls_close(conn);
		return PGRES_POLLING_FAILED;
	}

	if (!verify_peer_name_matches_certificate(conn))
	{
		pgtls_close(conn);
		return PGRES_POLLING_FAILED;
	}

	/* SSL handshake is complete */
	return PGRES_POLLING_OK;
}

/*
 *	Close SSL connection.
 */
void
pgtls_close(PGconn *conn)
{
	bool		destroy_needed = false;

	if (conn->ssl)
	{
		/*
		 * We can't destroy everything SSL-related here due to the possible
		 * later calls to OpenSSL routines which may need our thread
		 * callbacks, so set a flag here and check at the end.
		 */
		destroy_needed = true;

		SSL_shutdown(conn->ssl);
		SSL_free(conn->ssl);
		conn->ssl = NULL;
		conn->ssl_in_use = false;
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

	/*
	 * This will remove our SSL locking hooks, if this is the last SSL
	 * connection, which means we must wait to call it until after all SSL
	 * calls have been made, otherwise we can end up with a race condition and
	 * possible deadlocks.
	 *
	 * See comments above destroy_ssl_system().
	 */
	if (destroy_needed)
		destroy_ssl_system();
}


/*
 * Obtain reason string for passed SSL errcode
 *
 * ERR_get_error() is used by caller to get errcode to pass here.
 *
 * Some caution is needed here since ERR_reason_error_string will
 * return NULL if it doesn't recognize the error code.  We don't
 * want to return NULL ever.
 */
static char ssl_nomem[] = "out of memory allocating error description";

#define SSL_ERR_LEN 128

static char *
SSLerrmessage(unsigned long ecode)
{
	const char *errreason;
	char	   *errbuf;

	errbuf = malloc(SSL_ERR_LEN);
	if (!errbuf)
		return ssl_nomem;
	if (ecode == 0)
	{
		snprintf(errbuf, SSL_ERR_LEN, libpq_gettext("no SSL error reported"));
		return errbuf;
	}
	errreason = ERR_reason_error_string(ecode);
	if (errreason != NULL)
	{
		strlcpy(errbuf, errreason, SSL_ERR_LEN);
		return errbuf;
	}
	snprintf(errbuf, SSL_ERR_LEN, libpq_gettext("SSL error code %lu"), ecode);
	return errbuf;
}

static void
SSLerrfree(char *buf)
{
	if (buf != ssl_nomem)
		free(buf);
}

/* ------------------------------------------------------------ */
/*					SSL information functions					*/
/* ------------------------------------------------------------ */

int
PQsslInUse(PGconn *conn)
{
	if (!conn)
		return 0;
	return conn->ssl_in_use;
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

void *
PQsslStruct(PGconn *conn, const char *struct_name)
{
	if (!conn)
		return NULL;
	if (strcmp(struct_name, "OpenSSL") == 0)
		return conn->ssl;
	return NULL;
}

const char *const *
PQsslAttributeNames(PGconn *conn)
{
	static const char *const result[] = {
		"library",
		"key_bits",
		"cipher",
		"compression",
		"protocol",
		NULL
	};

	return result;
}

const char *
PQsslAttribute(PGconn *conn, const char *attribute_name)
{
	if (!conn)
		return NULL;
	if (conn->ssl == NULL)
		return NULL;

	if (strcmp(attribute_name, "library") == 0)
		return "OpenSSL";

	if (strcmp(attribute_name, "key_bits") == 0)
	{
		static char sslbits_str[10];
		int			sslbits;

		SSL_get_cipher_bits(conn->ssl, &sslbits);
		snprintf(sslbits_str, sizeof(sslbits_str), "%d", sslbits);
		return sslbits_str;
	}

	if (strcmp(attribute_name, "cipher") == 0)
		return SSL_get_cipher(conn->ssl);

	if (strcmp(attribute_name, "compression") == 0)
		return SSL_get_current_compression(conn->ssl) ? "on" : "off";

	if (strcmp(attribute_name, "protocol") == 0)
		return SSL_get_version(conn->ssl);

	return NULL;				/* unknown attribute */
}

/*
 * Private substitute BIO: this does the sending and receiving using
 * pqsecure_raw_write() and pqsecure_raw_read() instead, to allow those
 * functions to disable SIGPIPE and give better error messages on I/O errors.
 *
 * These functions are closely modelled on the standard socket BIO in OpenSSL;
 * see sock_read() and sock_write() in OpenSSL's crypto/bio/bss_sock.c.
 * XXX OpenSSL 1.0.1e considers many more errcodes than just EINTR as reasons
 * to retry; do we need to adopt their logic for that?
 */

static bool my_bio_initialized = false;
static BIO_METHOD my_bio_methods;

static int
my_sock_read(BIO *h, char *buf, int size)
{
	int			res;

	res = pqsecure_raw_read((PGconn *) h->ptr, buf, size);
	BIO_clear_retry_flags(h);
	if (res < 0)
	{
		/* If we were interrupted, tell caller to retry */
		switch (SOCK_ERRNO)
		{
#ifdef EAGAIN
			case EAGAIN:
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
			case EWOULDBLOCK:
#endif
			case EINTR:
				BIO_set_retry_read(h);
				break;

			default:
				break;
		}
	}

	return res;
}

static int
my_sock_write(BIO *h, const char *buf, int size)
{
	int			res;

	res = pqsecure_raw_write((PGconn *) h->ptr, buf, size);
	BIO_clear_retry_flags(h);
	if (res <= 0)
	{
		/* If we were interrupted, tell caller to retry */
		switch (SOCK_ERRNO)
		{
#ifdef EAGAIN
			case EAGAIN:
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
			case EWOULDBLOCK:
#endif
			case EINTR:
				BIO_set_retry_write(h);
				break;

			default:
				break;
		}
	}

	return res;
}

static BIO_METHOD *
my_BIO_s_socket(void)
{
	if (!my_bio_initialized)
	{
		memcpy(&my_bio_methods, BIO_s_socket(), sizeof(BIO_METHOD));
		my_bio_methods.bread = my_sock_read;
		my_bio_methods.bwrite = my_sock_write;
		my_bio_initialized = true;
	}
	return &my_bio_methods;
}

/* This should exactly match openssl's SSL_set_fd except for using my BIO */
static int
my_SSL_set_fd(PGconn *conn, int fd)
{
	int			ret = 0;
	BIO		   *bio = NULL;

	bio = BIO_new(my_BIO_s_socket());
	if (bio == NULL)
	{
		SSLerr(SSL_F_SSL_SET_FD, ERR_R_BUF_LIB);
		goto err;
	}
	/* Use 'ptr' to store pointer to PGconn */
	bio->ptr = conn;

	SSL_set_bio(conn->ssl, bio, bio);
	BIO_set_fd(bio, fd, BIO_NOCLOSE);
	ret = 1;
err:
	return ret;
}
