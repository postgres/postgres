/*-------------------------------------------------------------------------
 *
 * be-secure-openssl.c
 *	  functions for OpenSSL support in the backend.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/be-secure-openssl.c
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
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "common/string.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "utils/guc.h"
#include "utils/memutils.h"

/*
 * These SSL-related #includes must come after all system-provided headers.
 * This ensures that OpenSSL can take care of conflicts with Windows'
 * <wincrypt.h> by #undef'ing the conflicting macros.  (We don't directly
 * include <wincrypt.h>, but some other Windows headers do.)
 */
#include "common/openssl.h"
#include <openssl/bn.h>
#include <openssl/conf.h>
#include <openssl/dh.h>
#ifndef OPENSSL_NO_ECDH
#include <openssl/ec.h>
#endif
#include <openssl/x509v3.h>


/* default init hook can be overridden by a shared library */
static void default_openssl_tls_init(SSL_CTX *context, bool isServerStart);
openssl_tls_init_hook_typ openssl_tls_init_hook = default_openssl_tls_init;

static int	port_bio_read(BIO *h, char *buf, int size);
static int	port_bio_write(BIO *h, const char *buf, int size);
static BIO_METHOD *port_bio_method(void);
static int	ssl_set_port_bio(Port *port);

static DH  *load_dh_file(char *filename, bool isServerStart);
static DH  *load_dh_buffer(const char *buffer, size_t len);
static int	ssl_external_passwd_cb(char *buf, int size, int rwflag, void *userdata);
static int	dummy_ssl_passwd_cb(char *buf, int size, int rwflag, void *userdata);
static int	verify_cb(int ok, X509_STORE_CTX *ctx);
static void info_cb(const SSL *ssl, int type, int args);
static int	alpn_cb(SSL *ssl,
					const unsigned char **out,
					unsigned char *outlen,
					const unsigned char *in,
					unsigned int inlen,
					void *userdata);
static bool initialize_dh(SSL_CTX *context, bool isServerStart);
static bool initialize_ecdh(SSL_CTX *context, bool isServerStart);
static const char *SSLerrmessageExt(unsigned long ecode, const char *replacement);
static const char *SSLerrmessage(unsigned long ecode);

static char *X509_NAME_to_cstring(X509_NAME *name);

static SSL_CTX *SSL_context = NULL;
static bool dummy_ssl_passwd_cb_called = false;
static bool ssl_is_server_start;

static int	ssl_protocol_version_to_openssl(int v);
static const char *ssl_protocol_version_to_string(int v);

/* for passing data back from verify_cb() */
static const char *cert_errdetail;

/* ------------------------------------------------------------ */
/*						 Public interface						*/
/* ------------------------------------------------------------ */

int
be_tls_init(bool isServerStart)
{
	SSL_CTX    *context;
	int			ssl_ver_min = -1;
	int			ssl_ver_max = -1;

	/*
	 * Create a new SSL context into which we'll load all the configuration
	 * settings.  If we fail partway through, we can avoid memory leakage by
	 * freeing this context; we don't install it as active until the end.
	 *
	 * We use SSLv23_method() because it can negotiate use of the highest
	 * mutually supported protocol version, while alternatives like
	 * TLSv1_2_method() permit only one specific version.  Note that we don't
	 * actually allow SSL v2 or v3, only TLS protocols (see below).
	 */
	context = SSL_CTX_new(SSLv23_method());
	if (!context)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errmsg("could not create SSL context: %s",
						SSLerrmessage(ERR_get_error()))));
		goto error;
	}

	/*
	 * Disable OpenSSL's moving-write-buffer sanity check, because it causes
	 * unnecessary failures in nonblocking send cases.
	 */
	SSL_CTX_set_mode(context, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

	/*
	 * Call init hook (usually to set password callback)
	 */
	(*openssl_tls_init_hook) (context, isServerStart);

	/* used by the callback */
	ssl_is_server_start = isServerStart;

	/*
	 * Load and verify server's certificate and private key
	 */
	if (SSL_CTX_use_certificate_chain_file(context, ssl_cert_file) != 1)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not load server certificate file \"%s\": %s",
						ssl_cert_file, SSLerrmessage(ERR_get_error()))));
		goto error;
	}

	if (!check_ssl_key_file_permissions(ssl_key_file, isServerStart))
		goto error;

	/*
	 * OK, try to load the private key file.
	 */
	dummy_ssl_passwd_cb_called = false;

	if (SSL_CTX_use_PrivateKey_file(context,
									ssl_key_file,
									SSL_FILETYPE_PEM) != 1)
	{
		if (dummy_ssl_passwd_cb_called)
			ereport(isServerStart ? FATAL : LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("private key file \"%s\" cannot be reloaded because it requires a passphrase",
							ssl_key_file)));
		else
			ereport(isServerStart ? FATAL : LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not load private key file \"%s\": %s",
							ssl_key_file, SSLerrmessage(ERR_get_error()))));
		goto error;
	}

	if (SSL_CTX_check_private_key(context) != 1)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("check of private key failed: %s",
						SSLerrmessage(ERR_get_error()))));
		goto error;
	}

	if (ssl_min_protocol_version)
	{
		ssl_ver_min = ssl_protocol_version_to_openssl(ssl_min_protocol_version);

		if (ssl_ver_min == -1)
		{
			ereport(isServerStart ? FATAL : LOG,
			/*- translator: first %s is a GUC option name, second %s is its value */
					(errmsg("\"%s\" setting \"%s\" not supported by this build",
							"ssl_min_protocol_version",
							GetConfigOption("ssl_min_protocol_version",
											false, false))));
			goto error;
		}

		if (!SSL_CTX_set_min_proto_version(context, ssl_ver_min))
		{
			ereport(isServerStart ? FATAL : LOG,
					(errmsg("could not set minimum SSL protocol version")));
			goto error;
		}
	}

	if (ssl_max_protocol_version)
	{
		ssl_ver_max = ssl_protocol_version_to_openssl(ssl_max_protocol_version);

		if (ssl_ver_max == -1)
		{
			ereport(isServerStart ? FATAL : LOG,
			/*- translator: first %s is a GUC option name, second %s is its value */
					(errmsg("\"%s\" setting \"%s\" not supported by this build",
							"ssl_max_protocol_version",
							GetConfigOption("ssl_max_protocol_version",
											false, false))));
			goto error;
		}

		if (!SSL_CTX_set_max_proto_version(context, ssl_ver_max))
		{
			ereport(isServerStart ? FATAL : LOG,
					(errmsg("could not set maximum SSL protocol version")));
			goto error;
		}
	}

	/* Check compatibility of min/max protocols */
	if (ssl_min_protocol_version &&
		ssl_max_protocol_version)
	{
		/*
		 * No need to check for invalid values (-1) for each protocol number
		 * as the code above would have already generated an error.
		 */
		if (ssl_ver_min > ssl_ver_max)
		{
			ereport(isServerStart ? FATAL : LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not set SSL protocol version range"),
					 errdetail("\"%s\" cannot be higher than \"%s\"",
							   "ssl_min_protocol_version",
							   "ssl_max_protocol_version")));
			goto error;
		}
	}

	/*
	 * Disallow SSL session tickets. OpenSSL use both stateful and stateless
	 * tickets for TLSv1.3, and stateless ticket for TLSv1.2. SSL_OP_NO_TICKET
	 * is available since 0.9.8f but only turns off stateless tickets. In
	 * order to turn off stateful tickets we need SSL_CTX_set_num_tickets,
	 * which is available since OpenSSL 1.1.1.  LibreSSL 3.5.4 (from OpenBSD
	 * 7.1) introduced this API for compatibility, but doesn't support session
	 * tickets at all so it's a no-op there.
	 */
#ifdef HAVE_SSL_CTX_SET_NUM_TICKETS
	SSL_CTX_set_num_tickets(context, 0);
#endif
	SSL_CTX_set_options(context, SSL_OP_NO_TICKET);

	/* disallow SSL session caching, too */
	SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);

	/* disallow SSL compression */
	SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION);

	/*
	 * Disallow SSL renegotiation.  This concerns only TLSv1.2 and older
	 * protocol versions, as TLSv1.3 has no support for renegotiation.
	 * SSL_OP_NO_RENEGOTIATION is available in OpenSSL since 1.1.0h (via a
	 * backport from 1.1.1). SSL_OP_NO_CLIENT_RENEGOTIATION is available in
	 * LibreSSL since 2.5.1 disallowing all client-initiated renegotiation
	 * (this is usually on by default).
	 */
#ifdef SSL_OP_NO_RENEGOTIATION
	SSL_CTX_set_options(context, SSL_OP_NO_RENEGOTIATION);
#endif
#ifdef SSL_OP_NO_CLIENT_RENEGOTIATION
	SSL_CTX_set_options(context, SSL_OP_NO_CLIENT_RENEGOTIATION);
#endif

	/* set up ephemeral DH and ECDH keys */
	if (!initialize_dh(context, isServerStart))
		goto error;
	if (!initialize_ecdh(context, isServerStart))
		goto error;

	/* set up the allowed cipher list for TLSv1.2 and below */
	if (SSL_CTX_set_cipher_list(context, SSLCipherList) != 1)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not set the TLSv1.2 cipher list (no valid ciphers available)")));
		goto error;
	}

	/*
	 * Set up the allowed cipher suites for TLSv1.3. If the GUC is an empty
	 * string we leave the allowed suites to be the OpenSSL default value.
	 */
	if (SSLCipherSuites[0])
	{
		/* set up the allowed cipher suites */
		if (SSL_CTX_set_ciphersuites(context, SSLCipherSuites) != 1)
		{
			ereport(isServerStart ? FATAL : LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not set the TLSv1.3 cipher suites (no valid ciphers available)")));
			goto error;
		}
	}

	/* Let server choose order */
	if (SSLPreferServerCiphers)
		SSL_CTX_set_options(context, SSL_OP_CIPHER_SERVER_PREFERENCE);

	/*
	 * Load CA store, so we can verify client certificates if needed.
	 */
	if (ssl_ca_file[0])
	{
		STACK_OF(X509_NAME) * root_cert_list;

		if (SSL_CTX_load_verify_locations(context, ssl_ca_file, NULL) != 1 ||
			(root_cert_list = SSL_load_client_CA_file(ssl_ca_file)) == NULL)
		{
			ereport(isServerStart ? FATAL : LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not load root certificate file \"%s\": %s",
							ssl_ca_file, SSLerrmessage(ERR_get_error()))));
			goto error;
		}

		/*
		 * Tell OpenSSL to send the list of root certs we trust to clients in
		 * CertificateRequests.  This lets a client with a keystore select the
		 * appropriate client certificate to send to us.  Also, this ensures
		 * that the SSL context will "own" the root_cert_list and remember to
		 * free it when no longer needed.
		 */
		SSL_CTX_set_client_CA_list(context, root_cert_list);

		/*
		 * Always ask for SSL client cert, but don't fail if it's not
		 * presented.  We might fail such connections later, depending on what
		 * we find in pg_hba.conf.
		 */
		SSL_CTX_set_verify(context,
						   (SSL_VERIFY_PEER |
							SSL_VERIFY_CLIENT_ONCE),
						   verify_cb);
	}

	/*----------
	 * Load the Certificate Revocation List (CRL).
	 * http://searchsecurity.techtarget.com/sDefinition/0,,sid14_gci803160,00.html
	 *----------
	 */
	if (ssl_crl_file[0] || ssl_crl_dir[0])
	{
		X509_STORE *cvstore = SSL_CTX_get_cert_store(context);

		if (cvstore)
		{
			/* Set the flags to check against the complete CRL chain */
			if (X509_STORE_load_locations(cvstore,
										  ssl_crl_file[0] ? ssl_crl_file : NULL,
										  ssl_crl_dir[0] ? ssl_crl_dir : NULL)
				== 1)
			{
				X509_STORE_set_flags(cvstore,
									 X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
			}
			else if (ssl_crl_dir[0] == 0)
			{
				ereport(isServerStart ? FATAL : LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("could not load SSL certificate revocation list file \"%s\": %s",
								ssl_crl_file, SSLerrmessage(ERR_get_error()))));
				goto error;
			}
			else if (ssl_crl_file[0] == 0)
			{
				ereport(isServerStart ? FATAL : LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("could not load SSL certificate revocation list directory \"%s\": %s",
								ssl_crl_dir, SSLerrmessage(ERR_get_error()))));
				goto error;
			}
			else
			{
				ereport(isServerStart ? FATAL : LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("could not load SSL certificate revocation list file \"%s\" or directory \"%s\": %s",
								ssl_crl_file, ssl_crl_dir,
								SSLerrmessage(ERR_get_error()))));
				goto error;
			}
		}
	}

	/*
	 * Success!  Replace any existing SSL_context.
	 */
	if (SSL_context)
		SSL_CTX_free(SSL_context);

	SSL_context = context;

	/*
	 * Set flag to remember whether CA store has been loaded into SSL_context.
	 */
	if (ssl_ca_file[0])
		ssl_loaded_verify_locations = true;
	else
		ssl_loaded_verify_locations = false;

	return 0;

	/* Clean up by releasing working context. */
error:
	if (context)
		SSL_CTX_free(context);
	return -1;
}

void
be_tls_destroy(void)
{
	if (SSL_context)
		SSL_CTX_free(SSL_context);
	SSL_context = NULL;
	ssl_loaded_verify_locations = false;
}

int
be_tls_open_server(Port *port)
{
	int			r;
	int			err;
	int			waitfor;
	unsigned long ecode;
	bool		give_proto_hint;

	Assert(!port->ssl);
	Assert(!port->peer);

	if (!SSL_context)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not initialize SSL connection: SSL context not set up")));
		return -1;
	}

	/* set up debugging/info callback */
	SSL_CTX_set_info_callback(SSL_context, info_cb);

	/* enable ALPN */
	SSL_CTX_set_alpn_select_cb(SSL_context, alpn_cb, port);

	if (!(port->ssl = SSL_new(SSL_context)))
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not initialize SSL connection: %s",
						SSLerrmessage(ERR_get_error()))));
		return -1;
	}
	if (!ssl_set_port_bio(port))
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not set SSL socket: %s",
						SSLerrmessage(ERR_get_error()))));
		return -1;
	}
	port->ssl_in_use = true;

aloop:

	/*
	 * Prepare to call SSL_get_error() by clearing thread's OpenSSL error
	 * queue.  In general, the current thread's error queue must be empty
	 * before the TLS/SSL I/O operation is attempted, or SSL_get_error() will
	 * not work reliably.  An extension may have failed to clear the
	 * per-thread error queue following another call to an OpenSSL I/O
	 * routine.
	 */
	errno = 0;
	ERR_clear_error();
	r = SSL_accept(port->ssl);
	if (r <= 0)
	{
		err = SSL_get_error(port->ssl, r);

		/*
		 * Other clients of OpenSSL in the backend may fail to call
		 * ERR_get_error(), but we always do, so as to not cause problems for
		 * OpenSSL clients that don't call ERR_clear_error() defensively.  Be
		 * sure that this happens by calling now. SSL_get_error() relies on
		 * the OpenSSL per-thread error queue being intact, so this is the
		 * earliest possible point ERR_get_error() may be called.
		 */
		ecode = ERR_get_error();
		switch (err)
		{
			case SSL_ERROR_WANT_READ:
			case SSL_ERROR_WANT_WRITE:
				/* not allowed during connection establishment */
				Assert(!port->noblock);

				/*
				 * No need to care about timeouts/interrupts here. At this
				 * point authentication_timeout still employs
				 * StartupPacketTimeoutHandler() which directly exits.
				 */
				if (err == SSL_ERROR_WANT_READ)
					waitfor = WL_SOCKET_READABLE | WL_EXIT_ON_PM_DEATH;
				else
					waitfor = WL_SOCKET_WRITEABLE | WL_EXIT_ON_PM_DEATH;

				(void) WaitLatchOrSocket(NULL, waitfor, port->sock, 0,
										 WAIT_EVENT_SSL_OPEN_SERVER);
				goto aloop;
			case SSL_ERROR_SYSCALL:
				if (r < 0 && errno != 0)
					ereport(COMMERROR,
							(errcode_for_socket_access(),
							 errmsg("could not accept SSL connection: %m")));
				else
					ereport(COMMERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg("could not accept SSL connection: EOF detected")));
				break;
			case SSL_ERROR_SSL:
				switch (ERR_GET_REASON(ecode))
				{
						/*
						 * UNSUPPORTED_PROTOCOL, WRONG_VERSION_NUMBER, and
						 * TLSV1_ALERT_PROTOCOL_VERSION have been observed
						 * when trying to communicate with an old OpenSSL
						 * library, or when the client and server specify
						 * disjoint protocol ranges.  NO_PROTOCOLS_AVAILABLE
						 * occurs if there's a local misconfiguration (which
						 * can happen despite our checks, if openssl.cnf
						 * injects a limit we didn't account for).  It's not
						 * very clear what would make OpenSSL return the other
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
#endif
#ifdef SSL_R_VERSION_TOO_LOW
					case SSL_R_VERSION_TOO_LOW:
#endif
						give_proto_hint = true;
						break;
					default:
						give_proto_hint = false;
						break;
				}
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("could not accept SSL connection: %s",
								SSLerrmessage(ecode)),
						 cert_errdetail ? errdetail_internal("%s", cert_errdetail) : 0,
						 give_proto_hint ?
						 errhint("This may indicate that the client does not support any SSL protocol version between %s and %s.",
								 ssl_min_protocol_version ?
								 ssl_protocol_version_to_string(ssl_min_protocol_version) :
								 MIN_OPENSSL_TLS_VERSION,
								 ssl_max_protocol_version ?
								 ssl_protocol_version_to_string(ssl_max_protocol_version) :
								 MAX_OPENSSL_TLS_VERSION) : 0));
				cert_errdetail = NULL;
				break;
			case SSL_ERROR_ZERO_RETURN:
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("could not accept SSL connection: EOF detected")));
				break;
			default:
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("unrecognized SSL error code: %d",
								err)));
				break;
		}
		return -1;
	}

	/* Get the protocol selected by ALPN */
	port->alpn_used = false;
	{
		const unsigned char *selected;
		unsigned int len;

		SSL_get0_alpn_selected(port->ssl, &selected, &len);

		/* If ALPN is used, check that we negotiated the expected protocol */
		if (selected != NULL)
		{
			if (len == strlen(PG_ALPN_PROTOCOL) &&
				memcmp(selected, PG_ALPN_PROTOCOL, strlen(PG_ALPN_PROTOCOL)) == 0)
			{
				port->alpn_used = true;
			}
			else
			{
				/* shouldn't happen */
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("received SSL connection request with unexpected ALPN protocol")));
			}
		}
	}

	/* Get client certificate, if available. */
	port->peer = SSL_get_peer_certificate(port->ssl);

	/* and extract the Common Name and Distinguished Name from it. */
	port->peer_cn = NULL;
	port->peer_dn = NULL;
	port->peer_cert_valid = false;
	if (port->peer != NULL)
	{
		int			len;
		X509_NAME  *x509name = X509_get_subject_name(port->peer);
		char	   *peer_dn;
		BIO		   *bio = NULL;
		BUF_MEM    *bio_buf = NULL;

		len = X509_NAME_get_text_by_NID(x509name, NID_commonName, NULL, 0);
		if (len != -1)
		{
			char	   *peer_cn;

			peer_cn = MemoryContextAlloc(TopMemoryContext, len + 1);
			r = X509_NAME_get_text_by_NID(x509name, NID_commonName, peer_cn,
										  len + 1);
			peer_cn[len] = '\0';
			if (r != len)
			{
				/* shouldn't happen */
				pfree(peer_cn);
				return -1;
			}

			/*
			 * Reject embedded NULLs in certificate common name to prevent
			 * attacks like CVE-2009-4034.
			 */
			if (len != strlen(peer_cn))
			{
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("SSL certificate's common name contains embedded null")));
				pfree(peer_cn);
				return -1;
			}

			port->peer_cn = peer_cn;
		}

		bio = BIO_new(BIO_s_mem());
		if (!bio)
		{
			if (port->peer_cn != NULL)
			{
				pfree(port->peer_cn);
				port->peer_cn = NULL;
			}
			return -1;
		}

		/*
		 * RFC2253 is the closest thing to an accepted standard format for
		 * DNs. We have documented how to produce this format from a
		 * certificate. It uses commas instead of slashes for delimiters,
		 * which make regular expression matching a bit easier. Also note that
		 * it prints the Subject fields in reverse order.
		 */
		if (X509_NAME_print_ex(bio, x509name, 0, XN_FLAG_RFC2253) == -1 ||
			BIO_get_mem_ptr(bio, &bio_buf) <= 0)
		{
			BIO_free(bio);
			if (port->peer_cn != NULL)
			{
				pfree(port->peer_cn);
				port->peer_cn = NULL;
			}
			return -1;
		}
		peer_dn = MemoryContextAlloc(TopMemoryContext, bio_buf->length + 1);
		memcpy(peer_dn, bio_buf->data, bio_buf->length);
		len = bio_buf->length;
		BIO_free(bio);
		peer_dn[len] = '\0';
		if (len != strlen(peer_dn))
		{
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("SSL certificate's distinguished name contains embedded null")));
			pfree(peer_dn);
			if (port->peer_cn != NULL)
			{
				pfree(port->peer_cn);
				port->peer_cn = NULL;
			}
			return -1;
		}

		port->peer_dn = peer_dn;

		port->peer_cert_valid = true;
	}

	return 0;
}

void
be_tls_close(Port *port)
{
	if (port->ssl)
	{
		SSL_shutdown(port->ssl);
		SSL_free(port->ssl);
		port->ssl = NULL;
		port->ssl_in_use = false;
	}

	if (port->peer)
	{
		X509_free(port->peer);
		port->peer = NULL;
	}

	if (port->peer_cn)
	{
		pfree(port->peer_cn);
		port->peer_cn = NULL;
	}

	if (port->peer_dn)
	{
		pfree(port->peer_dn);
		port->peer_dn = NULL;
	}
}

ssize_t
be_tls_read(Port *port, void *ptr, size_t len, int *waitfor)
{
	ssize_t		n;
	int			err;
	unsigned long ecode;

	errno = 0;
	ERR_clear_error();
	n = SSL_read(port->ssl, ptr, len);
	err = SSL_get_error(port->ssl, n);
	ecode = (err != SSL_ERROR_NONE || n < 0) ? ERR_get_error() : 0;
	switch (err)
	{
		case SSL_ERROR_NONE:
			/* a-ok */
			break;
		case SSL_ERROR_WANT_READ:
			*waitfor = WL_SOCKET_READABLE;
			errno = EWOULDBLOCK;
			n = -1;
			break;
		case SSL_ERROR_WANT_WRITE:
			*waitfor = WL_SOCKET_WRITEABLE;
			errno = EWOULDBLOCK;
			n = -1;
			break;
		case SSL_ERROR_SYSCALL:
			/* leave it to caller to ereport the value of errno */
			if (n != -1 || errno == 0)
			{
				errno = ECONNRESET;
				n = -1;
			}
			break;
		case SSL_ERROR_SSL:
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("SSL error: %s", SSLerrmessage(ecode))));
			errno = ECONNRESET;
			n = -1;
			break;
		case SSL_ERROR_ZERO_RETURN:
			/* connection was cleanly shut down by peer */
			n = 0;
			break;
		default:
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unrecognized SSL error code: %d",
							err)));
			errno = ECONNRESET;
			n = -1;
			break;
	}

	return n;
}

ssize_t
be_tls_write(Port *port, const void *ptr, size_t len, int *waitfor)
{
	ssize_t		n;
	int			err;
	unsigned long ecode;

	errno = 0;
	ERR_clear_error();
	n = SSL_write(port->ssl, ptr, len);
	err = SSL_get_error(port->ssl, n);
	ecode = (err != SSL_ERROR_NONE || n < 0) ? ERR_get_error() : 0;
	switch (err)
	{
		case SSL_ERROR_NONE:
			/* a-ok */
			break;
		case SSL_ERROR_WANT_READ:
			*waitfor = WL_SOCKET_READABLE;
			errno = EWOULDBLOCK;
			n = -1;
			break;
		case SSL_ERROR_WANT_WRITE:
			*waitfor = WL_SOCKET_WRITEABLE;
			errno = EWOULDBLOCK;
			n = -1;
			break;
		case SSL_ERROR_SYSCALL:

			/*
			 * Leave it to caller to ereport the value of errno.  However, if
			 * errno is still zero then assume it's a read EOF situation, and
			 * report ECONNRESET.  (This seems possible because SSL_write can
			 * also do reads.)
			 */
			if (n != -1 || errno == 0)
			{
				errno = ECONNRESET;
				n = -1;
			}
			break;
		case SSL_ERROR_SSL:
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("SSL error: %s", SSLerrmessage(ecode))));
			errno = ECONNRESET;
			n = -1;
			break;
		case SSL_ERROR_ZERO_RETURN:

			/*
			 * the SSL connection was closed, leave it to the caller to
			 * ereport it
			 */
			errno = ECONNRESET;
			n = -1;
			break;
		default:
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unrecognized SSL error code: %d",
							err)));
			errno = ECONNRESET;
			n = -1;
			break;
	}

	return n;
}

/* ------------------------------------------------------------ */
/*						Internal functions						*/
/* ------------------------------------------------------------ */

/*
 * Private substitute BIO: this does the sending and receiving using send() and
 * recv() instead. This is so that we can enable and disable interrupts
 * just while calling recv(). We cannot have interrupts occurring while
 * the bulk of OpenSSL runs, because it uses malloc() and possibly other
 * non-reentrant libc facilities. We also need to call send() and recv()
 * directly so it gets passed through the socket/signals layer on Win32.
 *
 * These functions are closely modelled on the standard socket BIO in OpenSSL;
 * see sock_read() and sock_write() in OpenSSL's crypto/bio/bss_sock.c.
 */

static BIO_METHOD *port_bio_method_ptr = NULL;

static int
port_bio_read(BIO *h, char *buf, int size)
{
	int			res = 0;
	Port	   *port = (Port *) BIO_get_data(h);

	if (buf != NULL)
	{
		res = secure_raw_read(port, buf, size);
		BIO_clear_retry_flags(h);
		port->last_read_was_eof = res == 0;
		if (res <= 0)
		{
			/* If we were interrupted, tell caller to retry */
			if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)
			{
				BIO_set_retry_read(h);
			}
		}
	}

	return res;
}

static int
port_bio_write(BIO *h, const char *buf, int size)
{
	int			res = 0;

	res = secure_raw_write(((Port *) BIO_get_data(h)), buf, size);
	BIO_clear_retry_flags(h);
	if (res <= 0)
	{
		/* If we were interrupted, tell caller to retry */
		if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)
		{
			BIO_set_retry_write(h);
		}
	}

	return res;
}

static long
port_bio_ctrl(BIO *h, int cmd, long num, void *ptr)
{
	long		res;
	Port	   *port = (Port *) BIO_get_data(h);

	switch (cmd)
	{
		case BIO_CTRL_EOF:

			/*
			 * This should not be needed. port_bio_read already has a way to
			 * signal EOF to OpenSSL. However, OpenSSL made an undocumented,
			 * backwards-incompatible change and now expects EOF via BIO_ctrl.
			 * See https://github.com/openssl/openssl/issues/8208
			 */
			res = port->last_read_was_eof;
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
port_bio_method(void)
{
	if (!port_bio_method_ptr)
	{
		int			my_bio_index;

		my_bio_index = BIO_get_new_index();
		if (my_bio_index == -1)
			return NULL;
		my_bio_index |= BIO_TYPE_SOURCE_SINK;
		port_bio_method_ptr = BIO_meth_new(my_bio_index, "PostgreSQL backend socket");
		if (!port_bio_method_ptr)
			return NULL;
		if (!BIO_meth_set_write(port_bio_method_ptr, port_bio_write) ||
			!BIO_meth_set_read(port_bio_method_ptr, port_bio_read) ||
			!BIO_meth_set_ctrl(port_bio_method_ptr, port_bio_ctrl))
		{
			BIO_meth_free(port_bio_method_ptr);
			port_bio_method_ptr = NULL;
			return NULL;
		}
	}
	return port_bio_method_ptr;
}

static int
ssl_set_port_bio(Port *port)
{
	BIO		   *bio;
	BIO_METHOD *bio_method;

	bio_method = port_bio_method();
	if (bio_method == NULL)
		return 0;

	bio = BIO_new(bio_method);
	if (bio == NULL)
		return 0;

	BIO_set_data(bio, port);
	BIO_set_init(bio, 1);

	SSL_set_bio(port->ssl, bio, bio);
	return 1;
}

/*
 *	Load precomputed DH parameters.
 *
 *	To prevent "downgrade" attacks, we perform a number of checks
 *	to verify that the DBA-generated DH parameters file contains
 *	what we expect it to contain.
 */
static DH  *
load_dh_file(char *filename, bool isServerStart)
{
	FILE	   *fp;
	DH		   *dh = NULL;
	int			codes;

	/* attempt to open file.  It's not an error if it doesn't exist. */
	if ((fp = AllocateFile(filename, "r")) == NULL)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode_for_file_access(),
				 errmsg("could not open DH parameters file \"%s\": %m",
						filename)));
		return NULL;
	}

	dh = PEM_read_DHparams(fp, NULL, NULL, NULL);
	FreeFile(fp);

	if (dh == NULL)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not load DH parameters file: %s",
						SSLerrmessage(ERR_get_error()))));
		return NULL;
	}

	/* make sure the DH parameters are usable */
	if (DH_check(dh, &codes) == 0)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid DH parameters: %s",
						SSLerrmessage(ERR_get_error()))));
		DH_free(dh);
		return NULL;
	}
	if (codes & DH_CHECK_P_NOT_PRIME)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid DH parameters: p is not prime")));
		DH_free(dh);
		return NULL;
	}
	if ((codes & DH_NOT_SUITABLE_GENERATOR) &&
		(codes & DH_CHECK_P_NOT_SAFE_PRIME))
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid DH parameters: neither suitable generator or safe prime")));
		DH_free(dh);
		return NULL;
	}

	return dh;
}

/*
 *	Load hardcoded DH parameters.
 *
 *	If DH parameters cannot be loaded from a specified file, we can load
 *	the hardcoded DH parameters supplied with the backend to prevent
 *	problems.
 */
static DH  *
load_dh_buffer(const char *buffer, size_t len)
{
	BIO		   *bio;
	DH		   *dh = NULL;

	bio = BIO_new_mem_buf(buffer, len);
	if (bio == NULL)
		return NULL;
	dh = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
	if (dh == NULL)
		ereport(DEBUG2,
				(errmsg_internal("DH load buffer: %s",
								 SSLerrmessage(ERR_get_error()))));
	BIO_free(bio);

	return dh;
}

/*
 *	Passphrase collection callback using ssl_passphrase_command
 */
static int
ssl_external_passwd_cb(char *buf, int size, int rwflag, void *userdata)
{
	/* same prompt as OpenSSL uses internally */
	const char *prompt = "Enter PEM pass phrase:";

	Assert(rwflag == 0);

	return run_ssl_passphrase_command(prompt, ssl_is_server_start, buf, size);
}

/*
 * Dummy passphrase callback
 *
 * If OpenSSL is told to use a passphrase-protected server key, by default
 * it will issue a prompt on /dev/tty and try to read a key from there.
 * That's no good during a postmaster SIGHUP cycle, not to mention SSL context
 * reload in an EXEC_BACKEND postmaster child.  So override it with this dummy
 * function that just returns an empty passphrase, guaranteeing failure.
 */
static int
dummy_ssl_passwd_cb(char *buf, int size, int rwflag, void *userdata)
{
	/* Set flag to change the error message we'll report */
	dummy_ssl_passwd_cb_called = true;
	/* And return empty string */
	Assert(size > 0);
	buf[0] = '\0';
	return 0;
}

/*
 * Examines the provided certificate name, and if it's too long to log or
 * contains unprintable ASCII, escapes and truncates it. The return value is
 * always a new palloc'd string. (The input string is still modified in place,
 * for ease of implementation.)
 */
static char *
prepare_cert_name(char *name)
{
	size_t		namelen = strlen(name);
	char	   *truncated = name;

	/*
	 * Common Names are 64 chars max, so for a common case where the CN is the
	 * last field, we can still print the longest possible CN with a
	 * 7-character prefix (".../CN=[64 chars]"), for a reasonable limit of 71
	 * characters.
	 */
#define MAXLEN 71

	if (namelen > MAXLEN)
	{
		/*
		 * Keep the end of the name, not the beginning, since the most
		 * specific field is likely to give users the most information.
		 */
		truncated = name + namelen - MAXLEN;
		truncated[0] = truncated[1] = truncated[2] = '.';
		namelen = MAXLEN;
	}

#undef MAXLEN

	return pg_clean_ascii(truncated, 0);
}

/*
 *	Certificate verification callback
 *
 *	This callback allows us to examine intermediate problems during
 *	verification, for later logging.
 *
 *	This callback also allows us to override the default acceptance
 *	criteria (e.g., accepting self-signed or expired certs), but
 *	for now we accept the default checks.
 */
static int
verify_cb(int ok, X509_STORE_CTX *ctx)
{
	int			depth;
	int			errcode;
	const char *errstring;
	StringInfoData str;
	X509	   *cert;

	if (ok)
	{
		/* Nothing to do for the successful case. */
		return ok;
	}

	/* Pull all the information we have on the verification failure. */
	depth = X509_STORE_CTX_get_error_depth(ctx);
	errcode = X509_STORE_CTX_get_error(ctx);
	errstring = X509_verify_cert_error_string(errcode);

	initStringInfo(&str);
	appendStringInfo(&str,
					 _("Client certificate verification failed at depth %d: %s."),
					 depth, errstring);

	cert = X509_STORE_CTX_get_current_cert(ctx);
	if (cert)
	{
		char	   *subject,
				   *issuer;
		char	   *sub_prepared,
				   *iss_prepared;
		char	   *serialno;
		ASN1_INTEGER *sn;
		BIGNUM	   *b;

		/*
		 * Get the Subject and Issuer for logging, but don't let maliciously
		 * huge certs flood the logs, and don't reflect non-ASCII bytes into
		 * it either.
		 */
		subject = X509_NAME_to_cstring(X509_get_subject_name(cert));
		sub_prepared = prepare_cert_name(subject);
		pfree(subject);

		issuer = X509_NAME_to_cstring(X509_get_issuer_name(cert));
		iss_prepared = prepare_cert_name(issuer);
		pfree(issuer);

		/*
		 * Pull the serial number, too, in case a Subject is still ambiguous.
		 * This mirrors be_tls_get_peer_serial().
		 */
		sn = X509_get_serialNumber(cert);
		b = ASN1_INTEGER_to_BN(sn, NULL);
		serialno = BN_bn2dec(b);

		appendStringInfoChar(&str, '\n');
		appendStringInfo(&str,
						 _("Failed certificate data (unverified): subject \"%s\", serial number %s, issuer \"%s\"."),
						 sub_prepared, serialno ? serialno : _("unknown"),
						 iss_prepared);

		BN_free(b);
		OPENSSL_free(serialno);
		pfree(iss_prepared);
		pfree(sub_prepared);
	}

	/* Store our detail message to be logged later. */
	cert_errdetail = str.data;

	return ok;
}

/*
 *	This callback is used to copy SSL information messages
 *	into the PostgreSQL log.
 */
static void
info_cb(const SSL *ssl, int type, int args)
{
	const char *desc;

	desc = SSL_state_string_long(ssl);

	switch (type)
	{
		case SSL_CB_HANDSHAKE_START:
			ereport(DEBUG4,
					(errmsg_internal("SSL: handshake start: \"%s\"", desc)));
			break;
		case SSL_CB_HANDSHAKE_DONE:
			ereport(DEBUG4,
					(errmsg_internal("SSL: handshake done: \"%s\"", desc)));
			break;
		case SSL_CB_ACCEPT_LOOP:
			ereport(DEBUG4,
					(errmsg_internal("SSL: accept loop: \"%s\"", desc)));
			break;
		case SSL_CB_ACCEPT_EXIT:
			ereport(DEBUG4,
					(errmsg_internal("SSL: accept exit (%d): \"%s\"", args, desc)));
			break;
		case SSL_CB_CONNECT_LOOP:
			ereport(DEBUG4,
					(errmsg_internal("SSL: connect loop: \"%s\"", desc)));
			break;
		case SSL_CB_CONNECT_EXIT:
			ereport(DEBUG4,
					(errmsg_internal("SSL: connect exit (%d): \"%s\"", args, desc)));
			break;
		case SSL_CB_READ_ALERT:
			ereport(DEBUG4,
					(errmsg_internal("SSL: read alert (0x%04x): \"%s\"", args, desc)));
			break;
		case SSL_CB_WRITE_ALERT:
			ereport(DEBUG4,
					(errmsg_internal("SSL: write alert (0x%04x): \"%s\"", args, desc)));
			break;
	}
}

/* See pqcomm.h comments on OpenSSL implementation of ALPN (RFC 7301) */
static const unsigned char alpn_protos[] = PG_ALPN_PROTOCOL_VECTOR;

/*
 * Server callback for ALPN negotiation. We use the standard "helper" function
 * even though currently we only accept one value.
 */
static int
alpn_cb(SSL *ssl,
		const unsigned char **out,
		unsigned char *outlen,
		const unsigned char *in,
		unsigned int inlen,
		void *userdata)
{
	/*
	 * Why does OpenSSL provide a helper function that requires a nonconst
	 * vector when the callback is declared to take a const vector? What are
	 * we to do with that?
	 */
	int			retval;

	Assert(userdata != NULL);
	Assert(out != NULL);
	Assert(outlen != NULL);
	Assert(in != NULL);

	retval = SSL_select_next_proto((unsigned char **) out, outlen,
								   alpn_protos, sizeof(alpn_protos),
								   in, inlen);
	if (*out == NULL || *outlen > sizeof(alpn_protos) || *outlen <= 0)
		return SSL_TLSEXT_ERR_NOACK;	/* can't happen */

	if (retval == OPENSSL_NPN_NEGOTIATED)
		return SSL_TLSEXT_ERR_OK;
	else
	{
		/*
		 * The client doesn't support our protocol.  Reject the connection
		 * with TLS "no_application_protocol" alert, per RFC 7301.
		 */
		return SSL_TLSEXT_ERR_ALERT_FATAL;
	}
}


/*
 * Set DH parameters for generating ephemeral DH keys.  The
 * DH parameters can take a long time to compute, so they must be
 * precomputed.
 *
 * Since few sites will bother to create a parameter file, we also
 * provide a fallback to the parameters provided by the OpenSSL
 * project.
 *
 * These values can be static (once loaded or computed) since the
 * OpenSSL library can efficiently generate random keys from the
 * information provided.
 */
static bool
initialize_dh(SSL_CTX *context, bool isServerStart)
{
	DH		   *dh = NULL;

	SSL_CTX_set_options(context, SSL_OP_SINGLE_DH_USE);

	if (ssl_dh_params_file[0])
		dh = load_dh_file(ssl_dh_params_file, isServerStart);
	if (!dh)
		dh = load_dh_buffer(FILE_DH2048, sizeof(FILE_DH2048));
	if (!dh)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("DH: could not load DH parameters")));
		return false;
	}

	if (SSL_CTX_set_tmp_dh(context, dh) != 1)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("DH: could not set DH parameters: %s",
						SSLerrmessage(ERR_get_error()))));
		DH_free(dh);
		return false;
	}

	DH_free(dh);
	return true;
}

/*
 * Set ECDH parameters for generating ephemeral Elliptic Curve DH
 * keys.  This is much simpler than the DH parameters, as we just
 * need to provide the name of the curve to OpenSSL.
 */
static bool
initialize_ecdh(SSL_CTX *context, bool isServerStart)
{
#ifndef OPENSSL_NO_ECDH
	if (SSL_CTX_set1_groups_list(context, SSLECDHCurve) != 1)
	{
		/*
		 * OpenSSL 3.3.0 introduced proper error messages for group parsing
		 * errors, earlier versions returns "no SSL error reported" which is
		 * far from helpful. For older versions, we replace with a better
		 * error message. Injecting the error into the OpenSSL error queue
		 * need APIs from OpenSSL 3.0.
		 */
		ereport(isServerStart ? FATAL : LOG,
				errcode(ERRCODE_CONFIG_FILE_ERROR),
				errmsg("could not set group names specified in ssl_groups: %s",
					   SSLerrmessageExt(ERR_get_error(),
										_("No valid groups found"))),
				errhint("Ensure that each group name is spelled correctly and supported by the installed version of OpenSSL."));
		return false;
	}
#endif

	return true;
}

/*
 * Obtain reason string for passed SSL errcode with replacement
 *
 * The error message supplied in replacement will be used in case the error
 * code from OpenSSL is 0, else the error message from SSLerrmessage() will
 * be returned.
 *
 * Not all versions of OpenSSL place an error on the queue even for failing
 * operations, which will yield "no SSL error reported" by SSLerrmessage. This
 * function can be used to ensure that a proper error message is displayed for
 * versions reporting no error, while using the OpenSSL error via SSLerrmessage
 * for versions where there is one.
 */
static const char *
SSLerrmessageExt(unsigned long ecode, const char *replacement)
{
	if (ecode == 0)
		return replacement;
	else
		return SSLerrmessage(ecode);
}

/*
 * Obtain reason string for passed SSL errcode
 *
 * ERR_get_error() is used by caller to get errcode to pass here.
 *
 * Some caution is needed here since ERR_reason_error_string will return NULL
 * if it doesn't recognize the error code, or (in OpenSSL >= 3) if the code
 * represents a system errno value.  We don't want to return NULL ever.
 */
static const char *
SSLerrmessage(unsigned long ecode)
{
	const char *errreason;
	static char errbuf[36];

	if (ecode == 0)
		return _("no SSL error reported");
	errreason = ERR_reason_error_string(ecode);
	if (errreason != NULL)
		return errreason;

	/*
	 * In OpenSSL 3.0.0 and later, ERR_reason_error_string does not map system
	 * errno values anymore.  (See OpenSSL source code for the explanation.)
	 * We can cover that shortcoming with this bit of code.  Older OpenSSL
	 * versions don't have the ERR_SYSTEM_ERROR macro, but that's okay because
	 * they don't have the shortcoming either.
	 */
#ifdef ERR_SYSTEM_ERROR
	if (ERR_SYSTEM_ERROR(ecode))
		return strerror(ERR_GET_REASON(ecode));
#endif

	/* No choice but to report the numeric ecode */
	snprintf(errbuf, sizeof(errbuf), _("SSL error code %lu"), ecode);
	return errbuf;
}

int
be_tls_get_cipher_bits(Port *port)
{
	int			bits;

	if (port->ssl)
	{
		SSL_get_cipher_bits(port->ssl, &bits);
		return bits;
	}
	else
		return 0;
}

const char *
be_tls_get_version(Port *port)
{
	if (port->ssl)
		return SSL_get_version(port->ssl);
	else
		return NULL;
}

const char *
be_tls_get_cipher(Port *port)
{
	if (port->ssl)
		return SSL_get_cipher(port->ssl);
	else
		return NULL;
}

void
be_tls_get_peer_subject_name(Port *port, char *ptr, size_t len)
{
	if (port->peer)
		strlcpy(ptr, X509_NAME_to_cstring(X509_get_subject_name(port->peer)), len);
	else
		ptr[0] = '\0';
}

void
be_tls_get_peer_issuer_name(Port *port, char *ptr, size_t len)
{
	if (port->peer)
		strlcpy(ptr, X509_NAME_to_cstring(X509_get_issuer_name(port->peer)), len);
	else
		ptr[0] = '\0';
}

void
be_tls_get_peer_serial(Port *port, char *ptr, size_t len)
{
	if (port->peer)
	{
		ASN1_INTEGER *serial;
		BIGNUM	   *b;
		char	   *decimal;

		serial = X509_get_serialNumber(port->peer);
		b = ASN1_INTEGER_to_BN(serial, NULL);
		decimal = BN_bn2dec(b);

		BN_free(b);
		strlcpy(ptr, decimal, len);
		OPENSSL_free(decimal);
	}
	else
		ptr[0] = '\0';
}

char *
be_tls_get_certificate_hash(Port *port, size_t *len)
{
	X509	   *server_cert;
	char	   *cert_hash;
	const EVP_MD *algo_type = NULL;
	unsigned char hash[EVP_MAX_MD_SIZE];	/* size for SHA-512 */
	unsigned int hash_size;
	int			algo_nid;

	*len = 0;
	server_cert = SSL_get_certificate(port->ssl);
	if (server_cert == NULL)
		return NULL;

	/*
	 * Get the signature algorithm of the certificate to determine the hash
	 * algorithm to use for the result.  Prefer X509_get_signature_info(),
	 * introduced in OpenSSL 1.1.1, which can handle RSA-PSS signatures.
	 */
#if HAVE_X509_GET_SIGNATURE_INFO
	if (!X509_get_signature_info(server_cert, &algo_nid, NULL, NULL, NULL))
#else
	if (!OBJ_find_sigid_algs(X509_get_signature_nid(server_cert),
							 &algo_nid, NULL))
#endif
		elog(ERROR, "could not determine server certificate signature algorithm");

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
				elog(ERROR, "could not find digest for NID %s",
					 OBJ_nid2sn(algo_nid));
			break;
	}

	/* generate and save the certificate hash */
	if (!X509_digest(server_cert, algo_type, hash, &hash_size))
		elog(ERROR, "could not generate server certificate hash");

	cert_hash = palloc(hash_size);
	memcpy(cert_hash, hash, hash_size);
	*len = hash_size;

	return cert_hash;
}

/*
 * Convert an X509 subject name to a cstring.
 *
 */
static char *
X509_NAME_to_cstring(X509_NAME *name)
{
	BIO		   *membuf = BIO_new(BIO_s_mem());
	int			i,
				nid,
				count = X509_NAME_entry_count(name);
	X509_NAME_ENTRY *e;
	ASN1_STRING *v;
	const char *field_name;
	size_t		size;
	char		nullterm;
	char	   *sp;
	char	   *dp;
	char	   *result;

	if (membuf == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create BIO")));

	(void) BIO_set_close(membuf, BIO_CLOSE);
	for (i = 0; i < count; i++)
	{
		e = X509_NAME_get_entry(name, i);
		nid = OBJ_obj2nid(X509_NAME_ENTRY_get_object(e));
		if (nid == NID_undef)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not get NID for ASN1_OBJECT object")));
		v = X509_NAME_ENTRY_get_data(e);
		field_name = OBJ_nid2sn(nid);
		if (field_name == NULL)
			field_name = OBJ_nid2ln(nid);
		if (field_name == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not convert NID %d to an ASN1_OBJECT structure", nid)));
		BIO_printf(membuf, "/%s=", field_name);
		ASN1_STRING_print_ex(membuf, v,
							 ((ASN1_STRFLGS_RFC2253 & ~ASN1_STRFLGS_ESC_MSB)
							  | ASN1_STRFLGS_UTF8_CONVERT));
	}

	/* ensure null termination of the BIO's content */
	nullterm = '\0';
	BIO_write(membuf, &nullterm, 1);
	size = BIO_get_mem_data(membuf, &sp);
	dp = pg_any_to_server(sp, size - 1, PG_UTF8);

	result = pstrdup(dp);
	if (dp != sp)
		pfree(dp);
	if (BIO_free(membuf) != 1)
		elog(ERROR, "could not free OpenSSL BIO structure");

	return result;
}

/*
 * Convert TLS protocol version GUC enum to OpenSSL values
 *
 * This is a straightforward one-to-one mapping, but doing it this way makes
 * the definitions of ssl_min_protocol_version and ssl_max_protocol_version
 * independent of OpenSSL availability and version.
 *
 * If a version is passed that is not supported by the current OpenSSL
 * version, then we return -1.  If a nonnegative value is returned,
 * subsequent code can assume it's working with a supported version.
 *
 * Note: this is rather similar to libpq's routine in fe-secure-openssl.c,
 * so make sure to update both routines if changing this one.
 */
static int
ssl_protocol_version_to_openssl(int v)
{
	switch (v)
	{
		case PG_TLS_ANY:
			return 0;
		case PG_TLS1_VERSION:
			return TLS1_VERSION;
		case PG_TLS1_1_VERSION:
#ifdef TLS1_1_VERSION
			return TLS1_1_VERSION;
#else
			break;
#endif
		case PG_TLS1_2_VERSION:
#ifdef TLS1_2_VERSION
			return TLS1_2_VERSION;
#else
			break;
#endif
		case PG_TLS1_3_VERSION:
#ifdef TLS1_3_VERSION
			return TLS1_3_VERSION;
#else
			break;
#endif
	}

	return -1;
}

/*
 * Likewise provide a mapping to strings.
 */
static const char *
ssl_protocol_version_to_string(int v)
{
	switch (v)
	{
		case PG_TLS_ANY:
			return "any";
		case PG_TLS1_VERSION:
			return "TLSv1";
		case PG_TLS1_1_VERSION:
			return "TLSv1.1";
		case PG_TLS1_2_VERSION:
			return "TLSv1.2";
		case PG_TLS1_3_VERSION:
			return "TLSv1.3";
	}

	return "(unrecognized)";
}


static void
default_openssl_tls_init(SSL_CTX *context, bool isServerStart)
{
	if (isServerStart)
	{
		if (ssl_passphrase_command[0])
			SSL_CTX_set_default_passwd_cb(context, ssl_external_passwd_cb);
	}
	else
	{
		if (ssl_passphrase_command[0] && ssl_passphrase_command_supports_reload)
			SSL_CTX_set_default_passwd_cb(context, ssl_external_passwd_cb);
		else

			/*
			 * If reloading and no external command is configured, override
			 * OpenSSL's default handling of passphrase-protected files,
			 * because we don't want to prompt for a passphrase in an
			 * already-running server.
			 */
			SSL_CTX_set_default_passwd_cb(context, dummy_ssl_passwd_cb);
	}
}
