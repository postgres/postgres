/*-------------------------------------------------------------------------
 *
 * fe-secure-openssl.c
 *	  OpenSSL support
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-secure-openssl.c
 *
 * NOTES
 *
 *	  We don't provide informational callbacks here (like
 *	  info_cb() in be-secure-openssl.c), since there's no good mechanism to
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
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

#include <sys/stat.h>

#ifdef WIN32
#include "pthread-win32.h"
#else
#include <pthread.h>
#endif

/*
 * These SSL-related #includes must come after all system-provided headers.
 * This ensures that OpenSSL can take care of conflicts with Windows'
 * <wincrypt.h> by #undef'ing the conflicting macros.  (We don't directly
 * include <wincrypt.h>, but some other Windows headers do.)
 */
#include "common/openssl.h"
#include <openssl/conf.h>
#ifdef USE_SSL_ENGINE
#include <openssl/engine.h>
#endif
#include <openssl/x509v3.h>


static int	verify_cb(int ok, X509_STORE_CTX *ctx);
static int	openssl_verify_peer_name_matches_certificate_name(PGconn *conn,
															  ASN1_STRING *name_entry,
															  char **store_name);
static int	openssl_verify_peer_name_matches_certificate_ip(PGconn *conn,
															ASN1_OCTET_STRING *addr_entry,
															char **store_name);
static int	initialize_SSL(PGconn *conn);
static PostgresPollingStatusType open_client_SSL(PGconn *conn);
static char *SSLerrmessage(unsigned long ecode);
static void SSLerrfree(char *buf);
static int	PQssl_passwd_cb(char *buf, int size, int rwflag, void *userdata);

static int	pgconn_bio_read(BIO *h, char *buf, int size);
static int	pgconn_bio_write(BIO *h, const char *buf, int size);
static BIO_METHOD *pgconn_bio_method(void);
static int	ssl_set_pgconn_bio(PGconn *conn);

static pthread_mutex_t ssl_config_mutex = PTHREAD_MUTEX_INITIALIZER;

static PQsslKeyPassHook_OpenSSL_type PQsslKeyPassHook = NULL;
static int	ssl_protocol_version_to_openssl(const char *protocol);

/* ------------------------------------------------------------ */
/*			 Procedures common to all secure sessions			*/
/* ------------------------------------------------------------ */

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
				appendPQExpBufferStr(&conn->errorMessage,
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
					libpq_append_conn_error(conn, "server closed the connection unexpectedly\n"
											"\tThis probably means the server terminated abnormally\n"
											"\tbefore or while processing the request.");
				else
					libpq_append_conn_error(conn, "SSL SYSCALL error: %s",
											SOCK_STRERROR(result_errno,
														  sebuf, sizeof(sebuf)));
			}
			else
			{
				libpq_append_conn_error(conn, "SSL SYSCALL error: EOF detected");
				/* assume the connection is broken */
				result_errno = ECONNRESET;
				n = -1;
			}
			break;
		case SSL_ERROR_SSL:
			{
				char	   *errm = SSLerrmessage(ecode);

				libpq_append_conn_error(conn, "SSL error: %s", errm);
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
			libpq_append_conn_error(conn, "SSL connection has been closed unexpectedly");
			result_errno = ECONNRESET;
			n = -1;
			break;
		default:
			libpq_append_conn_error(conn, "unrecognized SSL error code: %d", err);
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
				appendPQExpBufferStr(&conn->errorMessage,
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
					libpq_append_conn_error(conn, "server closed the connection unexpectedly\n"
											"\tThis probably means the server terminated abnormally\n"
											"\tbefore or while processing the request.");
				else
					libpq_append_conn_error(conn, "SSL SYSCALL error: %s",
											SOCK_STRERROR(result_errno,
														  sebuf, sizeof(sebuf)));
			}
			else
			{
				libpq_append_conn_error(conn, "SSL SYSCALL error: EOF detected");
				/* assume the connection is broken */
				result_errno = ECONNRESET;
				n = -1;
			}
			break;
		case SSL_ERROR_SSL:
			{
				char	   *errm = SSLerrmessage(ecode);

				libpq_append_conn_error(conn, "SSL error: %s", errm);
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
			libpq_append_conn_error(conn, "SSL connection has been closed unexpectedly");
			result_errno = ECONNRESET;
			n = -1;
			break;
		default:
			libpq_append_conn_error(conn, "unrecognized SSL error code: %d", err);
			/* assume the connection is broken */
			result_errno = ECONNRESET;
			n = -1;
			break;
	}

	/* ensure we return the intended errno to caller */
	SOCK_ERRNO_SET(result_errno);

	return n;
}

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
		libpq_append_conn_error(conn, "could not determine server certificate signature algorithm");
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
				libpq_append_conn_error(conn, "could not find digest for NID %s",
										OBJ_nid2sn(algo_nid));
				return NULL;
			}
			break;
	}

	if (!X509_digest(peer_cert, algo_type, hash, &hash_size))
	{
		libpq_append_conn_error(conn, "could not generate peer certificate hash");
		return NULL;
	}

	/* save result */
	cert_hash = malloc(hash_size);
	if (cert_hash == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return NULL;
	}
	memcpy(cert_hash, hash, hash_size);
	*len = hash_size;

	return cert_hash;
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

#ifdef HAVE_SSL_CTX_SET_CERT_CB
/*
 * Certificate selection callback
 *
 * This callback lets us choose the client certificate we send to the server
 * after seeing its CertificateRequest.  We only support sending a single
 * hard-coded certificate via sslcert, so we don't actually set any certificates
 * here; we just use it to record whether or not the server has actually asked
 * for one and whether we have one to send.
 */
static int
cert_cb(SSL *ssl, void *arg)
{
	PGconn	   *conn = arg;

	conn->ssl_cert_requested = true;

	/* Do we have a certificate loaded to send back? */
	if (SSL_get_certificate(ssl))
		conn->ssl_cert_sent = true;

	/*
	 * Tell OpenSSL that the callback succeeded; we're not required to
	 * actually make any changes to the SSL handle.
	 */
	return 1;
}
#endif

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
		libpq_append_conn_error(conn, "SSL certificate's name entry is missing");
		return -1;
	}

	/*
	 * GEN_DNS can be only IA5String, equivalent to US ASCII.
	 */
	namedata = ASN1_STRING_get0_data(name_entry);
	len = ASN1_STRING_length(name_entry);

	/* OK to cast from unsigned to plain char, since it's all ASCII. */
	return pq_verify_peer_name_matches_certificate_name(conn, (const char *) namedata, len, store_name);
}

/*
 * OpenSSL-specific wrapper around
 * pq_verify_peer_name_matches_certificate_ip(), converting the
 * ASN1_OCTET_STRING into a plain C string.
 */
static int
openssl_verify_peer_name_matches_certificate_ip(PGconn *conn,
												ASN1_OCTET_STRING *addr_entry,
												char **store_name)
{
	int			len;
	const unsigned char *addrdata;

	/* Should not happen... */
	if (addr_entry == NULL)
	{
		libpq_append_conn_error(conn, "SSL certificate's address entry is missing");
		return -1;
	}

	/*
	 * GEN_IPADD is an OCTET STRING containing an IP address in network byte
	 * order.
	 */
	addrdata = ASN1_STRING_get0_data(addr_entry);
	len = ASN1_STRING_length(addr_entry);

	return pq_verify_peer_name_matches_certificate_ip(conn, addrdata, len, store_name);
}

static bool
is_ip_address(const char *host)
{
	struct in_addr dummy4;
#ifdef HAVE_INET_PTON
	struct in6_addr dummy6;
#endif

	return inet_aton(host, &dummy4)
#ifdef HAVE_INET_PTON
		|| (inet_pton(AF_INET6, host, &dummy6) == 1)
#endif
		;
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
	STACK_OF(GENERAL_NAME) * peer_san;
	int			i;
	int			rc = 0;
	char	   *host = conn->connhost[conn->whichhost].host;
	int			host_type;
	bool		check_cn = true;

	Assert(host && host[0]);	/* should be guaranteed by caller */

	/*
	 * We try to match the NSS behavior here, which is a slight departure from
	 * the spec but seems to make more intuitive sense:
	 *
	 * If connhost contains a DNS name, and the certificate's SANs contain any
	 * dNSName entries, then we'll ignore the Subject Common Name entirely;
	 * otherwise, we fall back to checking the CN. (This behavior matches the
	 * RFC.)
	 *
	 * If connhost contains an IP address, and the SANs contain iPAddress
	 * entries, we again ignore the CN. Otherwise, we allow the CN to match,
	 * EVEN IF there is a dNSName in the SANs. (RFC 6125 prohibits this: "A
	 * client MUST NOT seek a match for a reference identifier of CN-ID if the
	 * presented identifiers include a DNS-ID, SRV-ID, URI-ID, or any
	 * application-specific identifier types supported by the client.")
	 *
	 * NOTE: Prior versions of libpq did not consider iPAddress entries at
	 * all, so this new behavior might break a certificate that has different
	 * IP addresses in the Subject CN and the SANs.
	 */
	if (is_ip_address(host))
		host_type = GEN_IPADD;
	else
		host_type = GEN_DNS;

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
			char	   *alt_name = NULL;

			if (name->type == host_type)
			{
				/*
				 * This SAN is of the same type (IP or DNS) as our host name,
				 * so don't allow a fallback check of the CN.
				 */
				check_cn = false;
			}

			if (name->type == GEN_DNS)
			{
				(*names_examined)++;
				rc = openssl_verify_peer_name_matches_certificate_name(conn,
																	   name->d.dNSName,
																	   &alt_name);
			}
			else if (name->type == GEN_IPADD)
			{
				(*names_examined)++;
				rc = openssl_verify_peer_name_matches_certificate_ip(conn,
																	 name->d.iPAddress,
																	 &alt_name);
			}

			if (alt_name)
			{
				if (!*first_name)
					*first_name = alt_name;
				else
					free(alt_name);
			}

			if (rc != 0)
			{
				/*
				 * Either we hit an error or a match, and either way we should
				 * not fall back to the CN.
				 */
				check_cn = false;
				break;
			}
		}
		sk_GENERAL_NAME_pop_free(peer_san, GENERAL_NAME_free);
	}

	/*
	 * If there is no subjectAltName extension of the matching type, check the
	 * Common Name.
	 *
	 * (Per RFC 2818 and RFC 6125, if the subjectAltName extension of type
	 * dNSName is present, the CN must be ignored. We break this rule if host
	 * is an IP address; see the comment above.)
	 */
	if (check_cn)
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
				char	   *common_name = NULL;

				(*names_examined)++;
				rc = openssl_verify_peer_name_matches_certificate_name(conn,
																	   X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject_name, cn_index)),
																	   &common_name);

				if (common_name)
				{
					if (!*first_name)
						*first_name = common_name;
					else
						free(common_name);
				}
			}
		}
	}

	return rc;
}

/* See pqcomm.h comments on OpenSSL implementation of ALPN (RFC 7301) */
static unsigned char alpn_protos[] = PG_ALPN_PROTOCOL_VECTOR;

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
		!((conn->sslcrl && strlen(conn->sslcrl) > 0) ||
		  (conn->sslcrldir && strlen(conn->sslcrldir) > 0)))
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

		libpq_append_conn_error(conn, "could not create SSL context: %s", err);
		SSLerrfree(err);
		return -1;
	}

	/*
	 * Delegate the client cert password prompt to the libpq wrapper callback
	 * if any is defined.
	 *
	 * If the application hasn't installed its own and the sslpassword
	 * parameter is non-null, we install ours now to make sure we supply
	 * PGconn->sslpassword to OpenSSL instead of letting it prompt on stdin.
	 *
	 * This will replace OpenSSL's default PEM_def_callback (which prompts on
	 * stdin), but we're only setting it for this SSL context so it's
	 * harmless.
	 */
	if (PQsslKeyPassHook
		|| (conn->sslpassword && strlen(conn->sslpassword) > 0))
	{
		SSL_CTX_set_default_passwd_cb(SSL_context, PQssl_passwd_cb);
		SSL_CTX_set_default_passwd_cb_userdata(SSL_context, conn);
	}

#ifdef HAVE_SSL_CTX_SET_CERT_CB
	/* Set up a certificate selection callback. */
	SSL_CTX_set_cert_cb(SSL_context, cert_cb, conn);
#endif

	/* Disable old protocol versions */
	SSL_CTX_set_options(SSL_context, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

	/* Set the minimum and maximum protocol versions if necessary */
	if (conn->ssl_min_protocol_version &&
		strlen(conn->ssl_min_protocol_version) != 0)
	{
		int			ssl_min_ver;

		ssl_min_ver = ssl_protocol_version_to_openssl(conn->ssl_min_protocol_version);

		if (ssl_min_ver == -1)
		{
			libpq_append_conn_error(conn, "invalid value \"%s\" for minimum SSL protocol version",
									conn->ssl_min_protocol_version);
			SSL_CTX_free(SSL_context);
			return -1;
		}

		if (!SSL_CTX_set_min_proto_version(SSL_context, ssl_min_ver))
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			libpq_append_conn_error(conn, "could not set minimum SSL protocol version: %s", err);
			SSLerrfree(err);
			SSL_CTX_free(SSL_context);
			return -1;
		}
	}

	if (conn->ssl_max_protocol_version &&
		strlen(conn->ssl_max_protocol_version) != 0)
	{
		int			ssl_max_ver;

		ssl_max_ver = ssl_protocol_version_to_openssl(conn->ssl_max_protocol_version);

		if (ssl_max_ver == -1)
		{
			libpq_append_conn_error(conn, "invalid value \"%s\" for maximum SSL protocol version",
									conn->ssl_max_protocol_version);
			SSL_CTX_free(SSL_context);
			return -1;
		}

		if (!SSL_CTX_set_max_proto_version(SSL_context, ssl_max_ver))
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			libpq_append_conn_error(conn, "could not set maximum SSL protocol version: %s", err);
			SSLerrfree(err);
			SSL_CTX_free(SSL_context);
			return -1;
		}
	}

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

	if (strcmp(fnbuf, "system") == 0)
	{
		/*
		 * The "system" sentinel value indicates that we should load whatever
		 * root certificates are installed for use by OpenSSL; these locations
		 * differ by platform. Note that the default system locations may be
		 * further overridden by the SSL_CERT_DIR and SSL_CERT_FILE
		 * environment variables.
		 */
		if (SSL_CTX_set_default_verify_paths(SSL_context) != 1)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			libpq_append_conn_error(conn, "could not load system root certificate paths: %s",
									err);
			SSLerrfree(err);
			SSL_CTX_free(SSL_context);
			return -1;
		}
		have_rootcert = true;
	}
	else if (fnbuf[0] != '\0' &&
			 stat(fnbuf, &buf) == 0)
	{
		X509_STORE *cvstore;

		if (SSL_CTX_load_verify_locations(SSL_context, fnbuf, NULL) != 1)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			libpq_append_conn_error(conn, "could not read root certificate file \"%s\": %s",
									fnbuf, err);
			SSLerrfree(err);
			SSL_CTX_free(SSL_context);
			return -1;
		}

		if ((cvstore = SSL_CTX_get_cert_store(SSL_context)) != NULL)
		{
			char	   *fname = NULL;
			char	   *dname = NULL;

			if (conn->sslcrl && strlen(conn->sslcrl) > 0)
				fname = conn->sslcrl;
			if (conn->sslcrldir && strlen(conn->sslcrldir) > 0)
				dname = conn->sslcrldir;

			/* defaults to use the default CRL file */
			if (!fname && !dname && have_homedir)
			{
				snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, ROOT_CRL_FILE);
				fname = fnbuf;
			}

			/* Set the flags to check against the complete CRL chain */
			if ((fname || dname) &&
				X509_STORE_load_locations(cvstore, fname, dname) == 1)
			{
				X509_STORE_set_flags(cvstore,
									 X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
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
				libpq_append_conn_error(conn, "could not get home directory to locate root certificate file\n"
										"Either provide the file, use the system's trusted roots with sslrootcert=system, or change sslmode to disable server certificate verification.");
			else
				libpq_append_conn_error(conn, "root certificate file \"%s\" does not exist\n"
										"Either provide the file, use the system's trusted roots with sslrootcert=system, or change sslmode to disable server certificate verification.", fnbuf);
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

	if (conn->sslcertmode[0] == 'd')	/* disable */
	{
		/* don't send a client cert even if we have one */
		have_cert = false;
	}
	else if (fnbuf[0] == '\0')
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
			libpq_append_conn_error(conn, "could not open certificate file \"%s\": %s",
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

			libpq_append_conn_error(conn, "could not read certificate file \"%s\": %s",
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
		!ssl_set_pgconn_bio(conn))
	{
		char	   *err = SSLerrmessage(ERR_get_error());

		libpq_append_conn_error(conn, "could not establish SSL connection: %s", err);
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
	 * Set Server Name Indication (SNI), if enabled by connection parameters.
	 * Per RFC 6066, do not set it if the host is a literal IP address (IPv4
	 * or IPv6).
	 */
	if (conn->sslsni && conn->sslsni[0] == '1')
	{
		const char *host = conn->connhost[conn->whichhost].host;

		if (host && host[0] &&
			!(strspn(host, "0123456789.") == strlen(host) ||
			  strchr(host, ':')))
		{
			if (SSL_set_tlsext_host_name(conn->ssl, host) != 1)
			{
				char	   *err = SSLerrmessage(ERR_get_error());

				libpq_append_conn_error(conn, "could not set SSL Server Name Indication (SNI): %s", err);
				SSLerrfree(err);
				return -1;
			}
		}
	}

	/* Set ALPN */
	{
		int			retval;

		retval = SSL_set_alpn_protos(conn->ssl, alpn_protos, sizeof(alpn_protos));

		if (retval != 0)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			libpq_append_conn_error(conn, "could not set SSL ALPN extension: %s", err);
			SSLerrfree(err);
			return -1;
		}
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
			EVP_PKEY   *pkey;

			if (engine_str == NULL)
			{
				libpq_append_conn_error(conn, "out of memory");
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

				libpq_append_conn_error(conn, "could not load SSL engine \"%s\": %s",
										engine_str, err);
				SSLerrfree(err);
				free(engine_str);
				return -1;
			}

			if (ENGINE_init(conn->engine) == 0)
			{
				char	   *err = SSLerrmessage(ERR_get_error());

				libpq_append_conn_error(conn, "could not initialize SSL engine \"%s\": %s",
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

				libpq_append_conn_error(conn, "could not read private SSL key \"%s\" from engine \"%s\": %s",
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

				libpq_append_conn_error(conn, "could not load private SSL key \"%s\" from engine \"%s\": %s",
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
			if (errno == ENOENT)
				libpq_append_conn_error(conn, "certificate present, but not private key file \"%s\"",
										fnbuf);
			else
				libpq_append_conn_error(conn, "could not stat private key file \"%s\": %m",
										fnbuf);
			return -1;
		}

		/* Key file must be a regular file */
		if (!S_ISREG(buf.st_mode))
		{
			libpq_append_conn_error(conn, "private key file \"%s\" is not a regular file",
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
			libpq_append_conn_error(conn,
									"private key file \"%s\" has group or world access; file must have permissions u=rw (0600) or less if owned by the current user, or permissions u=rw,g=r (0640) or less if owned by root",
									fnbuf);
			return -1;
		}
#endif

		if (SSL_use_PrivateKey_file(conn->ssl, fnbuf, SSL_FILETYPE_PEM) != 1)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			/*
			 * We'll try to load the file in DER (binary ASN.1) format, and if
			 * that fails too, report the original error. This could mask
			 * issues where there's something wrong with a DER-format cert,
			 * but we'd have to duplicate openssl's format detection to be
			 * smarter than this. We can't just probe for a leading -----BEGIN
			 * because PEM can have leading non-matching lines and blanks.
			 * OpenSSL doesn't expose its get_name(...) and its PEM routines
			 * don't differentiate between failure modes in enough detail to
			 * let us tell the difference between "not PEM, try DER" and
			 * "wrong password".
			 */
			if (SSL_use_PrivateKey_file(conn->ssl, fnbuf, SSL_FILETYPE_ASN1) != 1)
			{
				libpq_append_conn_error(conn, "could not load private key file \"%s\": %s",
										fnbuf, err);
				SSLerrfree(err);
				return -1;
			}

			SSLerrfree(err);
		}
	}

	/* verify that the cert and key go together */
	if (have_cert &&
		SSL_check_private_key(conn->ssl) != 1)
	{
		char	   *err = SSLerrmessage(ERR_get_error());

		libpq_append_conn_error(conn, "certificate does not match private key file \"%s\": %s",
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
	 * Set compression option if necessary.
	 */
	if (conn->sslcompression && conn->sslcompression[0] == '0')
		SSL_set_options(conn->ssl, SSL_OP_NO_COMPRESSION);
	else
		SSL_clear_options(conn->ssl, SSL_OP_NO_COMPRESSION);

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
					unsigned long vcode;

					vcode = SSL_get_verify_result(conn->ssl);

					/*
					 * If we get an X509 error here for failing to load the
					 * local issuer cert, without an error in the socket layer
					 * it means that verification failed due to a missing
					 * system CA pool without it being a protocol error. We
					 * inspect the sslrootcert setting to ensure that the user
					 * was using the system CA pool. For other errors, log
					 * them using the normal SYSCALL logging.
					 */
					if (save_errno == 0 &&
						vcode == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY &&
						strcmp(conn->sslrootcert, "system") == 0)
						libpq_append_conn_error(conn, "SSL error: certificate verify failed: %s",
												X509_verify_cert_error_string(vcode));
					else if (r == -1 && save_errno != 0)
						libpq_append_conn_error(conn, "SSL SYSCALL error: %s",
												SOCK_STRERROR(save_errno, sebuf, sizeof(sebuf)));
					else
						libpq_append_conn_error(conn, "SSL SYSCALL error: EOF detected");
					pgtls_close(conn);
					return PGRES_POLLING_FAILED;
				}
			case SSL_ERROR_SSL:
				{
					char	   *err = SSLerrmessage(ecode);

					libpq_append_conn_error(conn, "SSL error: %s", err);
					SSLerrfree(err);
					switch (ERR_GET_REASON(ecode))
					{
							/*
							 * UNSUPPORTED_PROTOCOL, WRONG_VERSION_NUMBER, and
							 * TLSV1_ALERT_PROTOCOL_VERSION have been observed
							 * when trying to communicate with an old OpenSSL
							 * library, or when the client and server specify
							 * disjoint protocol ranges.
							 * NO_PROTOCOLS_AVAILABLE occurs if there's a
							 * local misconfiguration (which can happen
							 * despite our checks, if openssl.cnf injects a
							 * limit we didn't account for).  It's not very
							 * clear what would make OpenSSL return the other
							 * codes listed here, but a hint about protocol
							 * versions seems like it's appropriate for all.
							 */
						case SSL_R_NO_PROTOCOLS_AVAILABLE:
						case SSL_R_UNSUPPORTED_PROTOCOL:
						case SSL_R_BAD_PROTOCOL_VERSION_NUMBER:
						case SSL_R_UNKNOWN_PROTOCOL:
						case SSL_R_UNKNOWN_SSL_VERSION:
						case SSL_R_UNSUPPORTED_SSL_VERSION:
						case SSL_R_WRONG_SSL_VERSION:
						case SSL_R_WRONG_VERSION_NUMBER:
						case SSL_R_TLSV1_ALERT_PROTOCOL_VERSION:
#ifdef SSL_R_VERSION_TOO_HIGH
						case SSL_R_VERSION_TOO_HIGH:
						case SSL_R_VERSION_TOO_LOW:
#endif
							libpq_append_conn_error(conn, "This may indicate that the server does not support any SSL protocol version between %s and %s.",
													conn->ssl_min_protocol_version ?
													conn->ssl_min_protocol_version :
													MIN_OPENSSL_TLS_VERSION,
													conn->ssl_max_protocol_version ?
													conn->ssl_max_protocol_version :
													MAX_OPENSSL_TLS_VERSION);
							break;
						default:
							break;
					}
					pgtls_close(conn);
					return PGRES_POLLING_FAILED;
				}

			default:
				libpq_append_conn_error(conn, "unrecognized SSL error code: %d", err);
				pgtls_close(conn);
				return PGRES_POLLING_FAILED;
		}
	}

	/* ALPN is mandatory with direct SSL connections */
	if (conn->current_enc_method == ENC_SSL && conn->sslnegotiation[0] == 'd')
	{
		const unsigned char *selected;
		unsigned int len;

		SSL_get0_alpn_selected(conn->ssl, &selected, &len);

		if (selected == NULL)
		{
			libpq_append_conn_error(conn, "direct SSL connection was established without ALPN protocol negotiation extension");
			pgtls_close(conn);
			return PGRES_POLLING_FAILED;
		}

		/*
		 * We only support one protocol so that's what the negotiation should
		 * always choose, but doesn't hurt to check.
		 */
		if (len != strlen(PG_ALPN_PROTOCOL) ||
			memcmp(selected, PG_ALPN_PROTOCOL, strlen(PG_ALPN_PROTOCOL)) != 0)
		{
			libpq_append_conn_error(conn, "SSL connection was established with unexpected ALPN protocol");
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
		char	   *err = SSLerrmessage(ERR_get_error());

		libpq_append_conn_error(conn, "certificate could not be obtained: %s", err);
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
	if (conn->ssl_in_use)
	{
		if (conn->ssl)
		{
			/*
			 * We can't destroy everything SSL-related here due to the
			 * possible later calls to OpenSSL routines which may need our
			 * thread callbacks, so set a flag here and check at the end.
			 */

			SSL_shutdown(conn->ssl);
			SSL_free(conn->ssl);
			conn->ssl = NULL;
			conn->ssl_in_use = false;
			conn->ssl_handshake_started = false;
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
	 * Server aborted the connection with TLS "no_application_protocol" alert.
	 * The ERR_reason_error_string() function doesn't give any error string
	 * for that for some reason, so do it ourselves.  See
	 * https://github.com/openssl/openssl/issues/24300.  This is available in
	 * OpenSSL 1.1.0 and later, as well as in LibreSSL 3.4.3 (OpenBSD 7.0) and
	 * later.
	 */
#ifdef SSL_AD_NO_APPLICATION_PROTOCOL
	if (ERR_GET_LIB(ecode) == ERR_LIB_SSL &&
		ERR_GET_REASON(ecode) == SSL_AD_REASON_OFFSET + SSL_AD_NO_APPLICATION_PROTOCOL)
	{
		snprintf(errbuf, SSL_ERR_LEN, "no application protocol");
		return errbuf;
	}
#endif

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
	static const char *const openssl_attrs[] = {
		"library",
		"key_bits",
		"cipher",
		"compression",
		"protocol",
		"alpn",
		NULL
	};
	static const char *const empty_attrs[] = {NULL};

	if (!conn)
	{
		/* Return attributes of default SSL library */
		return openssl_attrs;
	}

	/* No attrs for unencrypted connection */
	if (conn->ssl == NULL)
		return empty_attrs;

	return openssl_attrs;
}

const char *
PQsslAttribute(PGconn *conn, const char *attribute_name)
{
	if (!conn)
	{
		/* PQsslAttribute(NULL, "library") reports the default SSL library */
		if (strcmp(attribute_name, "library") == 0)
			return "OpenSSL";
		return NULL;
	}

	/* All attributes read as NULL for a non-encrypted connection */
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

	if (strcmp(attribute_name, "alpn") == 0)
	{
		const unsigned char *data;
		unsigned int len;
		static char alpn_str[256];	/* alpn doesn't support longer than 255
									 * bytes */

		SSL_get0_alpn_selected(conn->ssl, &data, &len);
		if (data == NULL || len == 0 || len > sizeof(alpn_str) - 1)
			return "";
		memcpy(alpn_str, data, len);
		alpn_str[len] = 0;
		return alpn_str;
	}

	return NULL;				/* unknown attribute */
}

/*
 * Private substitute BIO: this does the sending and receiving using
 * pqsecure_raw_write() and pqsecure_raw_read() instead, to allow those
 * functions to disable SIGPIPE and give better error messages on I/O errors.
 *
 * These functions are closely modelled on the standard socket BIO in OpenSSL;
 * see sock_read() and sock_write() in OpenSSL's crypto/bio/bss_sock.c.
 */

/* protected by ssl_config_mutex */
static BIO_METHOD *pgconn_bio_method_ptr;

static int
pgconn_bio_read(BIO *h, char *buf, int size)
{
	PGconn	   *conn = (PGconn *) BIO_get_data(h);
	int			res;

	res = pqsecure_raw_read(conn, buf, size);
	BIO_clear_retry_flags(h);
	conn->last_read_was_eof = res == 0;
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

	if (res > 0)
		conn->ssl_handshake_started = true;

	return res;
}

static int
pgconn_bio_write(BIO *h, const char *buf, int size)
{
	int			res;

	res = pqsecure_raw_write((PGconn *) BIO_get_data(h), buf, size);
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

static long
pgconn_bio_ctrl(BIO *h, int cmd, long num, void *ptr)
{
	long		res;
	PGconn	   *conn = (PGconn *) BIO_get_data(h);

	switch (cmd)
	{
		case BIO_CTRL_EOF:

			/*
			 * This should not be needed. pgconn_bio_read already has a way to
			 * signal EOF to OpenSSL. However, OpenSSL made an undocumented,
			 * backwards-incompatible change and now expects EOF via BIO_ctrl.
			 * See https://github.com/openssl/openssl/issues/8208
			 */
			res = conn->last_read_was_eof;
			break;
		case BIO_CTRL_FLUSH:
			/* libssl expects all BIOs to support BIO_flush. */
			res = 1;
			break;
		default:
			res = 0;
			break;
	}

	return res;
}

static BIO_METHOD *
pgconn_bio_method(void)
{
	BIO_METHOD *res;

	if (pthread_mutex_lock(&ssl_config_mutex))
		return NULL;

	res = pgconn_bio_method_ptr;

	if (!pgconn_bio_method_ptr)
	{
		int			my_bio_index;

		my_bio_index = BIO_get_new_index();
		if (my_bio_index == -1)
			goto err;
		my_bio_index |= BIO_TYPE_SOURCE_SINK;
		res = BIO_meth_new(my_bio_index, "libpq socket");
		if (!res)
			goto err;

		/*
		 * As of this writing, these functions never fail. But check anyway,
		 * like OpenSSL's own examples do.
		 */
		if (!BIO_meth_set_write(res, pgconn_bio_write) ||
			!BIO_meth_set_read(res, pgconn_bio_read) ||
			!BIO_meth_set_ctrl(res, pgconn_bio_ctrl))
		{
			goto err;
		}
	}

	pgconn_bio_method_ptr = res;
	pthread_mutex_unlock(&ssl_config_mutex);
	return res;

err:
	if (res)
		BIO_meth_free(res);
	pthread_mutex_unlock(&ssl_config_mutex);
	return NULL;
}

static int
ssl_set_pgconn_bio(PGconn *conn)
{
	BIO		   *bio;
	BIO_METHOD *bio_method;

	bio_method = pgconn_bio_method();
	if (bio_method == NULL)
		return 0;

	bio = BIO_new(bio_method);
	if (bio == NULL)
		return 0;

	BIO_set_data(bio, conn);
	BIO_set_init(bio, 1);

	SSL_set_bio(conn->ssl, bio, bio);
	return 1;
}

/*
 * This is the default handler to return a client cert password from
 * conn->sslpassword. Apps may install it explicitly if they want to
 * prevent openssl from ever prompting on stdin.
 */
int
PQdefaultSSLKeyPassHook_OpenSSL(char *buf, int size, PGconn *conn)
{
	if (conn && conn->sslpassword)
	{
		if (strlen(conn->sslpassword) + 1 > size)
			fprintf(stderr, libpq_gettext("WARNING: sslpassword truncated\n"));
		strncpy(buf, conn->sslpassword, size);
		buf[size - 1] = '\0';
		return strlen(buf);
	}
	else
	{
		buf[0] = '\0';
		return 0;
	}
}

PQsslKeyPassHook_OpenSSL_type
PQgetSSLKeyPassHook_OpenSSL(void)
{
	return PQsslKeyPassHook;
}

void
PQsetSSLKeyPassHook_OpenSSL(PQsslKeyPassHook_OpenSSL_type hook)
{
	PQsslKeyPassHook = hook;
}

/*
 * Supply a password to decrypt a client certificate.
 *
 * This must match OpenSSL type pem_password_cb.
 */
static int
PQssl_passwd_cb(char *buf, int size, int rwflag, void *userdata)
{
	PGconn	   *conn = userdata;

	if (PQsslKeyPassHook)
		return PQsslKeyPassHook(buf, size, conn);
	else
		return PQdefaultSSLKeyPassHook_OpenSSL(buf, size, conn);
}

/*
 * Convert TLS protocol version string to OpenSSL values
 *
 * If a version is passed that is not supported by the current OpenSSL version,
 * then we return -1. If a non-negative value is returned, subsequent code can
 * assume it is working with a supported version.
 *
 * Note: this is rather similar to the backend routine in be-secure-openssl.c,
 * so make sure to update both routines if changing this one.
 */
static int
ssl_protocol_version_to_openssl(const char *protocol)
{
	if (pg_strcasecmp("TLSv1", protocol) == 0)
		return TLS1_VERSION;

#ifdef TLS1_1_VERSION
	if (pg_strcasecmp("TLSv1.1", protocol) == 0)
		return TLS1_1_VERSION;
#endif

#ifdef TLS1_2_VERSION
	if (pg_strcasecmp("TLSv1.2", protocol) == 0)
		return TLS1_2_VERSION;
#endif

#ifdef TLS1_3_VERSION
	if (pg_strcasecmp("TLSv1.3", protocol) == 0)
		return TLS1_3_VERSION;
#endif

	return -1;
}
