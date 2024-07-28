/*-------------------------------------------------------------------------
 *
 * fe-secure-openssl.c
 *	  OpenSSL support
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
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
#include "fe-secure-common.h"
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
#include <openssl/conf.h>
#ifdef USE_SSL_ENGINE
#include <openssl/engine.h>
#endif
#include <openssl/x509v3.h>

static int	verify_cb(int ok, X509_STORE_CTX *ctx);
static int	openssl_verify_peer_name_matches_certificate_name(PGconn *conn,
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

static bool ssl_lib_initialized = false;

#ifdef ENABLE_THREAD_SAFETY
static long ssl_open_connections = 0;

static pthread_mutex_t ssl_config_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif							/* ENABLE_THREAD_SAFETY */


/* ------------------------------------------------------------ */
/*			 Procedures common to all secure sessions			*/
/* ------------------------------------------------------------ */

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

PostgresPollingStatusType
pgtls_open_client(PGconn *conn)
{
	/* First time through? */
	if (conn->ssl == NULL)
	{
		/*
		 * Create a connection-specific SSL object, and load client
		 * certificate, private key, and trusted CA certs.
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

ssize_t
pgtls_read(PGconn *conn, void *ptr, size_t len)
{
	ssize_t		n;
	int			result_errno = 0;
	char		sebuf[PG_STRERROR_R_BUFLEN];
	int			err;
	unsigned long ecode;

rloop:

	/*
	 * Prepare to call SSL_get_error() by clearing thread's OpenSSL error
	 * queue.  In general, the current thread's error queue must be empty
	 * before the TLS/SSL I/O operation is attempted, or SSL_get_error() will
	 * not work reliably.  Since the possibility exists that other OpenSSL
	 * clients running in the same thread but not under our control will fail
	 * to call ERR_get_error() themselves (after their own I/O operations),
	 * pro-actively clear the per-thread error queue now.
	 */
	SOCK_ERRNO_SET(0);
	ERR_clear_error();
	n = SSL_read(conn->ssl, ptr, len);
	err = SSL_get_error(conn->ssl, n);

	/*
	 * Other clients of OpenSSL may fail to call ERR_get_error(), but we
	 * always do, so as to not cause problems for OpenSSL clients that don't
	 * call ERR_clear_error() defensively.  Be sure that this happens by
	 * calling now.  SSL_get_error() relies on the OpenSSL per-thread error
	 * queue being intact, so this is the earliest possible point
	 * ERR_get_error() may be called.
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
			if (n < 0 && SOCK_ERRNO != 0)
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

bool
pgtls_read_pending(PGconn *conn)
{
	return SSL_pending(conn->ssl) > 0;
}

ssize_t
pgtls_write(PGconn *conn, const void *ptr, size_t len)
{
	ssize_t		n;
	int			result_errno = 0;
	char		sebuf[PG_STRERROR_R_BUFLEN];
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

			/*
			 * If errno is still zero then assume it's a read EOF situation,
			 * and report EOF.  (This seems possible because SSL_write can
			 * also do reads.)
			 */
			if (n < 0 && SOCK_ERRNO != 0)
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

#if defined(HAVE_X509_GET_SIGNATURE_NID) || defined(HAVE_X509_GET_SIGNATURE_INFO)
char *
pgtls_get_peer_certificate_hash(PGconn *conn, size_t *len)
{
	X509	   *peer_cert;
	const EVP_MD *algo_type;
	unsigned char hash[EVP_MAX_MD_SIZE];	/* size for SHA-512 */
	unsigned int hash_size;
	int			algo_nid;
	char	   *cert_hash;

	*len = 0;

	if (!conn->peer)
		return NULL;

	peer_cert = conn->peer;

	/*
	 * Get the signature algorithm of the certificate to determine the hash
	 * algorithm to use for the result.  Prefer X509_get_signature_info(),
	 * introduced in OpenSSL 1.1.1, which can handle RSA-PSS signatures.
	 */
#if HAVE_X509_GET_SIGNATURE_INFO
	if (!X509_get_signature_info(peer_cert, &algo_nid, NULL, NULL, NULL))
#else
	if (!OBJ_find_sigid_algs(X509_get_signature_nid(peer_cert),
							 &algo_nid, NULL))
#endif
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("could not determine server certificate signature algorithm\n"));
		return NULL;
	}

	/*
	 * The TLS server's certificate bytes need to be hashed with SHA-256 if
	 * its signature algorithm is MD5 or SHA-1 as per RFC 5929
	 * (https://tools.ietf.org/html/rfc5929#section-4.1).  If something else
	 * is used, the same hash as the signature algorithm is used.
	 */
	switch (algo_nid)
	{
		case NID_md5:
		case NID_sha1:
			algo_type = EVP_sha256();
			break;
		default:
			algo_type = EVP_get_digestbynid(algo_nid);
			if (algo_type == NULL)
			{
				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("could not find digest for NID %s\n"),
								  OBJ_nid2sn(algo_nid));
				return NULL;
			}
			break;
	}

	if (!X509_digest(peer_cert, algo_type, hash, &hash_size))
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("could not generate peer certificate hash\n"));
		return NULL;
	}

	/* save result */
	cert_hash = malloc(hash_size);
	if (cert_hash == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("out of memory\n"));
		return NULL;
	}
	memcpy(cert_hash, hash, hash_size);
	*len = hash_size;

	return cert_hash;
}
#endif							/* HAVE_X509_GET_SIGNATURE_NID */

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
 * OpenSSL-specific wrapper around
 * pq_verify_peer_name_matches_certificate_name(), converting the ASN1_STRING
 * into a plain C string.
 */
static int
openssl_verify_peer_name_matches_certificate_name(PGconn *conn, ASN1_STRING *name_entry,
												  char **store_name)
{
	int			len;
	const unsigned char *namedata;

	/* Should not happen... */
	if (name_entry == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("SSL certificate's name entry is missing\n"));
		return -1;
	}

	/*
	 * GEN_DNS can be only IA5String, equivalent to US ASCII.
	 */
#ifdef HAVE_ASN1_STRING_GET0_DATA
	namedata = ASN1_STRING_get0_data(name_entry);
#else
	namedata = ASN1_STRING_data(name_entry);
#endif
	len = ASN1_STRING_length(name_entry);

	/* OK to cast from unsigned to plain char, since it's all ASCII. */
	return pq_verify_peer_name_matches_certificate_name(conn, (const char *) namedata, len, store_name);
}

/*
 *	Verify that the server certificate matches the hostname we connected to.
 *
 * The certificate's Common Name and Subject Alternative Names are considered.
 */
int
pgtls_verify_peer_name_matches_certificate_guts(PGconn *conn,
												int *names_examined,
												char **first_name)
{
	STACK_OF(GENERAL_NAME) *peer_san;
	int			i;
	int			rc = 0;

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

				(*names_examined)++;
				rc = openssl_verify_peer_name_matches_certificate_name(conn,
																	   name->d.dNSName,
																	   &alt_name);

				if (alt_name)
				{
					if (!*first_name)
						*first_name = alt_name;
					else
						free(alt_name);
				}
			}
			if (rc != 0)
				break;
		}
		sk_GENERAL_NAME_pop_free(peer_san, GENERAL_NAME_free);
	}

	/*
	 * If there is no subjectAltName extension of type dNSName, check the
	 * Common Name.
	 *
	 * (Per RFC 2818 and RFC 6125, if the subjectAltName extension of type
	 * dNSName is present, the CN must be ignored.)
	 */
	if (*names_examined == 0)
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
				(*names_examined)++;
				rc = openssl_verify_peer_name_matches_certificate_name(
																	   conn,
																	   X509_NAME_ENTRY_get_data(
																								X509_NAME_get_entry(subject_name, cn_index)),
																	   first_name);
			}
		}
	}

	return rc;
}

#if defined(ENABLE_THREAD_SAFETY) && defined(HAVE_CRYPTO_LOCK)
/*
 *	Callback functions for OpenSSL internal locking.  (OpenSSL 1.1.0
 *	does its own locking, and doesn't need these anymore.  The
 *	CRYPTO_lock() function was removed in 1.1.0, when the callbacks
 *	were made obsolete, so we assume that if CRYPTO_lock() exists,
 *	the callbacks are still required.)
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
#endif							/* ENABLE_THREAD_SAFETY && HAVE_CRYPTO_LOCK */

/*
 * Initialize SSL library.
 *
 * In threadsafe mode, this includes setting up libcrypto callback functions
 * to do thread locking.
 *
 * If the caller has told us (through PQinitOpenSSL) that he's taking care
 * of libcrypto, we expect that callbacks are already set, and won't try to
 * override it.
 */
int
pgtls_init(PGconn *conn)
{
#ifdef ENABLE_THREAD_SAFETY
	if (pthread_mutex_lock(&ssl_config_mutex))
		return -1;

#ifdef HAVE_CRYPTO_LOCK
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
#endif							/* HAVE_CRYPTO_LOCK */
#endif							/* ENABLE_THREAD_SAFETY */

	if (!ssl_lib_initialized)
	{
		if (pq_init_ssl_lib)
		{
#ifdef HAVE_OPENSSL_INIT_SSL
			OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, NULL);
#else
			OPENSSL_config(NULL);
			SSL_library_init();
			SSL_load_error_strings();
#endif
		}
		ssl_lib_initialized = true;
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
 *	we only need to remove them in this case. They are also not needed
 *	with OpenSSL 1.1.0 anymore.
 */
static void
destroy_ssl_system(void)
{
#if defined(ENABLE_THREAD_SAFETY) && defined(HAVE_CRYPTO_LOCK)
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
		 * We don't free the lock array. If we get another connection in this
		 * process, we will just re-use them with the existing mutexes.
		 *
		 * This means we leak a little memory on repeated load/unload of the
		 * library.
		 */
	}

	pthread_mutex_unlock(&ssl_config_mutex);
#endif
}

/*
 *	Create per-connection SSL object, and load the client certificate,
 *	private key, and trusted CA certs.
 *
 *	Returns 0 if OK, -1 on failure (with a message in conn->errorMessage).
 */
static int
initialize_SSL(PGconn *conn)
{
	SSL_CTX    *SSL_context;
	struct stat buf;
	char		homedir[MAXPGPATH];
	char		fnbuf[MAXPGPATH];
	char		sebuf[PG_STRERROR_R_BUFLEN];
	bool		have_homedir;
	bool		have_cert;
	bool		have_rootcert;

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
	else						/* won't need it */
		have_homedir = false;

	/*
	 * Create a new SSL_CTX object.
	 *
	 * We used to share a single SSL_CTX between all connections, but it was
	 * complicated if connections used different certificates. So now we
	 * create a separate context for each connection, and accept the overhead.
	 */
	SSL_context = SSL_CTX_new(SSLv23_method());
	if (!SSL_context)
	{
		char	   *err = SSLerrmessage(ERR_get_error());

		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("could not create SSL context: %s\n"),
						  err);
		SSLerrfree(err);
		return -1;
	}

	/* Disable old protocol versions */
	SSL_CTX_set_options(SSL_context, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

	/*
	 * Disable OpenSSL's moving-write-buffer sanity check, because it causes
	 * unnecessary failures in nonblocking send cases.
	 */
	SSL_CTX_set_mode(SSL_context, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

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

		if (SSL_CTX_load_verify_locations(SSL_context, fnbuf, NULL) != 1)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("could not read root certificate file \"%s\": %s\n"),
							  fnbuf, err);
			SSLerrfree(err);
			SSL_CTX_free(SSL_context);
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
				SSL_CTX_free(SSL_context);
				return -1;
#endif
			}
			/* if not found, silently ignore;  we do not require CRL */
			ERR_clear_error();
		}
		have_rootcert = true;
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
			SSL_CTX_free(SSL_context);
			return -1;
		}
		have_rootcert = false;
	}

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
							  fnbuf, strerror_r(errno, sebuf, sizeof(sebuf)));
			SSL_CTX_free(SSL_context);
			return -1;
		}
		have_cert = false;
	}
	else
	{
		/*
		 * Cert file exists, so load it. Since OpenSSL doesn't provide the
		 * equivalent of "SSL_use_certificate_chain_file", we have to load it
		 * into the SSL context, rather than the SSL object.
		 */
		if (SSL_CTX_use_certificate_chain_file(SSL_context, fnbuf) != 1)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("could not read certificate file \"%s\": %s\n"),
							  fnbuf, err);
			SSLerrfree(err);
			SSL_CTX_free(SSL_context);
			return -1;
		}

		/* need to load the associated private key, too */
		have_cert = true;
	}

	/*
	 * The SSL context is now loaded with the correct root and client
	 * certificates. Create a connection-specific SSL object. The private key
	 * is loaded directly into the SSL object. (We could load the private key
	 * into the context, too, but we have done it this way historically, and
	 * it doesn't really matter.)
	 */
	if (!(conn->ssl = SSL_new(SSL_context)) ||
		!SSL_set_app_data(conn->ssl, conn) ||
		!my_SSL_set_fd(conn, conn->sock))
	{
		char	   *err = SSLerrmessage(ERR_get_error());

		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("could not establish SSL connection: %s\n"),
						  err);
		SSLerrfree(err);
		SSL_CTX_free(SSL_context);
		return -1;
	}
	conn->ssl_in_use = true;

	/*
	 * SSL contexts are reference counted by OpenSSL. We can free it as soon
	 * as we have created the SSL object, and it will stick around for as long
	 * as it's actually needed.
	 */
	SSL_CTX_free(SSL_context);
	SSL_context = NULL;

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
			EVP_PKEY   *pkey;

			if (engine_str == NULL)
			{
				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("out of memory\n"));
				return -1;
			}

			/* cannot return NULL because we already checked before strdup */
			engine_colon = strchr(engine_str, ':');

			*engine_colon = '\0';	/* engine_str now has engine name */
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
#endif							/* USE_SSL_ENGINE */
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

		/* Key file must be a regular file */
		if (!S_ISREG(buf.st_mode))
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("private key file \"%s\" is not a regular file\n"),
							  fnbuf);
			return -1;
		}

		/*
		 * Refuse to load world-readable key files.  We accept root-owned
		 * files with mode 0640 or less, so that we can access system-wide
		 * certificates if we have a supplementary group membership that
		 * allows us to read 'em.  For files with non-root ownership, require
		 * mode 0600 or less.  We need not check the file's ownership exactly;
		 * if we're able to read it despite it having such restrictive
		 * permissions, it must have the right ownership.
		 *
		 * Note: be very careful about tightening these rules.  Some people
		 * expect, for example, that a client process running as root should
		 * be able to use a non-root-owned key file.
		 *
		 * Note that roughly similar checks are performed in
		 * src/backend/libpq/be-secure-common.c so any changes here may need
		 * to be made there as well.  However, this code caters for the case
		 * of current user == root, while that code does not.
		 *
		 * Ideally we would do similar permissions checks on Windows, but it
		 * is not clear how that would work since Unix-style permissions may
		 * not be available.
		 */
#if !defined(WIN32) && !defined(__CYGWIN__)
		if (buf.st_uid == 0 ?
			buf.st_mode & (S_IWGRP | S_IXGRP | S_IRWXO) :
			buf.st_mode & (S_IRWXG | S_IRWXO))
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("private key file \"%s\" has group or world access; file must have permissions u=rw (0600) or less if owned by the current user, or permissions u=rw,g=r (0640) or less if owned by root\n"),
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
	 * If a root cert was loaded, also set our certificate verification
	 * callback.
	 */
	if (have_rootcert)
		SSL_set_verify(conn->ssl, SSL_VERIFY_PEER, verify_cb);

	/*
	 * Set compression option if the OpenSSL version used supports it (from
	 * 1.0.0 on).
	 */
#ifdef SSL_OP_NO_COMPRESSION
	if (conn->sslcompression && conn->sslcompression[0] == '0')
		SSL_set_options(conn->ssl, SSL_OP_NO_COMPRESSION);

	/*
	 * Mainline OpenSSL introduced SSL_clear_options() before
	 * SSL_OP_NO_COMPRESSION, so this following #ifdef should not be
	 * necessary, but some old NetBSD version have a locally modified libssl
	 * that has SSL_OP_NO_COMPRESSION but not SSL_clear_options().
	 */
#ifdef HAVE_SSL_CLEAR_OPTIONS
	else
		SSL_clear_options(conn->ssl, SSL_OP_NO_COMPRESSION);
#endif
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

	SOCK_ERRNO_SET(0);
	ERR_clear_error();
	r = SSL_connect(conn->ssl);
	if (r <= 0)
	{
		int			save_errno = SOCK_ERRNO;
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
					char		sebuf[PG_STRERROR_R_BUFLEN];

					if (r == -1 && save_errno != 0)
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext("SSL SYSCALL error: %s\n"),
										  SOCK_STRERROR(save_errno, sebuf, sizeof(sebuf)));
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

	if (!pq_verify_peer_name_matches_certificate(conn))
	{
		pgtls_close(conn);
		return PGRES_POLLING_FAILED;
	}

	/* SSL handshake is complete */
	return PGRES_POLLING_OK;
}

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
 * The result must be freed after use, using SSLerrfree.
 *
 * Some caution is needed here since ERR_reason_error_string will return NULL
 * if it doesn't recognize the error code, or (in OpenSSL >= 3) if the code
 * represents a system errno value.  We don't want to return NULL ever.
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

	/*
	 * In OpenSSL 3.0.0 and later, ERR_reason_error_string does not map system
	 * errno values anymore.  (See OpenSSL source code for the explanation.)
	 * We can cover that shortcoming with this bit of code.  Older OpenSSL
	 * versions don't have the ERR_SYSTEM_ERROR macro, but that's okay because
	 * they don't have the shortcoming either.
	 */
#ifdef ERR_SYSTEM_ERROR
	if (ERR_SYSTEM_ERROR(ecode))
	{
		strerror_r(ERR_GET_REASON(ecode), errbuf, SSL_ERR_LEN);
		return errbuf;
	}
#endif

	/* No choice but to report the numeric ecode */
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
		static char sslbits_str[12];
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

/* protected by ssl_config_mutex */
static BIO_METHOD *my_bio_methods;

static int
my_sock_read(BIO *h, char *buf, int size)
{
	int			res;

	res = pqsecure_raw_read((PGconn *) BIO_get_app_data(h), buf, size);
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

	res = pqsecure_raw_write((PGconn *) BIO_get_app_data(h), buf, size);
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
	BIO_METHOD *res;

#ifdef ENABLE_THREAD_SAFETY
	if (pthread_mutex_lock(&ssl_config_mutex))
		return NULL;
#endif

	res = my_bio_methods;

	if (!my_bio_methods)
	{
		BIO_METHOD *biom = (BIO_METHOD *) BIO_s_socket();
#ifdef HAVE_BIO_METH_NEW
		int			my_bio_index;

		my_bio_index = BIO_get_new_index();
		if (my_bio_index == -1)
			goto err;
		my_bio_index |= (BIO_TYPE_DESCRIPTOR | BIO_TYPE_SOURCE_SINK);
		res = BIO_meth_new(my_bio_index, "libpq socket");
		if (!res)
			goto err;

		/*
		 * As of this writing, these functions never fail. But check anyway,
		 * like OpenSSL's own examples do.
		 */
		if (!BIO_meth_set_write(res, my_sock_write) ||
			!BIO_meth_set_read(res, my_sock_read) ||
			!BIO_meth_set_gets(res, BIO_meth_get_gets(biom)) ||
			!BIO_meth_set_puts(res, BIO_meth_get_puts(biom)) ||
			!BIO_meth_set_ctrl(res, BIO_meth_get_ctrl(biom)) ||
			!BIO_meth_set_create(res, BIO_meth_get_create(biom)) ||
			!BIO_meth_set_destroy(res, BIO_meth_get_destroy(biom)) ||
			!BIO_meth_set_callback_ctrl(res, BIO_meth_get_callback_ctrl(biom)))
		{
			goto err;
		}
#else
		res = malloc(sizeof(BIO_METHOD));
		if (!res)
			goto err;
		memcpy(res, biom, sizeof(BIO_METHOD));
		res->bread = my_sock_read;
		res->bwrite = my_sock_write;
#endif
	}

	my_bio_methods = res;

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&ssl_config_mutex);
#endif

	return res;

err:
#ifdef HAVE_BIO_METH_NEW
	if (res)
		BIO_meth_free(res);
#else
	if (res)
		free(res);
#endif

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&ssl_config_mutex);
#endif
	return NULL;
}

/* This should exactly match OpenSSL's SSL_set_fd except for using my BIO */
static int
my_SSL_set_fd(PGconn *conn, int fd)
{
	int			ret = 0;
	BIO		   *bio;
	BIO_METHOD *bio_method;

	bio_method = my_BIO_s_socket();
	if (bio_method == NULL)
	{
		SSLerr(SSL_F_SSL_SET_FD, ERR_R_BUF_LIB);
		goto err;
	}
	bio = BIO_new(bio_method);
	if (bio == NULL)
	{
		SSLerr(SSL_F_SSL_SET_FD, ERR_R_BUF_LIB);
		goto err;
	}
	BIO_set_app_data(bio, conn);

	SSL_set_bio(conn->ssl, bio, bio);
	BIO_set_fd(bio, fd, BIO_NOCLOSE);
	ret = 1;
err:
	return ret;
}
