/*-------------------------------------------------------------------------
 *
 * be-secure.c
 *	  functions related to setting up a secure connection to the frontend.
 *	  Secure connections are expected to provide confidentiality,
 *	  message integrity and endpoint authentication.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/be-secure.c,v 1.35 2003/07/01 13:49:47 tgl Exp $
 *
 *	  Since the server static private key ($DataDir/server.key)
 *	  will normally be stored unencrypted so that the database
 *	  backend can restart automatically, it is important that
 *	  we select an algorithm that continues to provide confidentiality
 *	  even if the attacker has the server's private key.  Empheral
 *	  DH (EDH) keys provide this, and in fact provide Perfect Forward
 *	  Secrecy (PFS) except for situations where the session can
 *	  be hijacked during a periodic handshake/renegotiation.
 *	  Even that backdoor can be closed if client certificates
 *	  are used (since the imposter will be unable to successfully
 *	  complete renegotiation).
 *
 *	  N.B., the static private key should still be protected to
 *	  the largest extent possible, to minimize the risk of
 *	  impersonations.
 *
 *	  Another benefit of EDH is that it allows the backend and
 *	  clients to use DSA keys.	DSA keys can only provide digital
 *	  signatures, not encryption, and are often acceptable in
 *	  jurisdictions where RSA keys are unacceptable.
 *
 *	  The downside to EDH is that it makes it impossible to
 *	  use ssldump(1) if there's a problem establishing an SSL
 *	  session.	In this case you'll need to temporarily disable
 *	  EDH by commenting out the callback.
 *
 *	  ...
 *
 *	  Because the risk of cryptanalysis increases as large
 *	  amounts of data are sent with the same session key, the
 *	  session keys are periodically renegotiated.
 *
 * PATCH LEVEL
 *	  milestone 1: fix basic coding errors
 *	  [*] existing SSL code pulled out of existing files.
 *	  [*] SSL_get_error() after SSL_read() and SSL_write(),
 *		  SSL_shutdown(), default to TLSv1.
 *
 *	  milestone 2: provide endpoint authentication (server)
 *	  [*] client verifies server cert
 *	  [*] client verifies server hostname
 *
 *	  milestone 3: improve confidentially, support perfect forward secrecy
 *	  [ ] use 'random' file, read from '/dev/urandom?'
 *	  [*] emphermal DH keys, default values
 *	  [*] periodic renegotiation
 *	  [*] private key permissions
 *
 *	  milestone 4: provide endpoint authentication (client)
 *	  [*] server verifies client certificates
 *
 *	  milestone 5: provide informational callbacks
 *	  [*] provide informational callbacks
 *
 *	  other changes
 *	  [ ] tcp-wrappers
 *	  [ ] more informative psql
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#include "libpq/libpq.h"
#include "miscadmin.h"

#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

#ifdef USE_SSL
#include <openssl/ssl.h>
#include <openssl/dh.h>
#endif

extern void ExitPostmaster(int);
extern void postmaster_error(const char *fmt,...);

#ifdef USE_SSL
static DH  *load_dh_file(int keylength);
static DH  *load_dh_buffer(const char *, size_t);
static DH  *tmp_dh_cb(SSL *s, int is_export, int keylength);
static int	verify_cb(int, X509_STORE_CTX *);
static void info_cb(const SSL *ssl, int type, int args);
static int	initialize_SSL(void);
static void destroy_SSL(void);
static int	open_server_SSL(Port *);
static void close_SSL(Port *);
static const char *SSLerrmessage(void);
#endif

#ifdef USE_SSL
/*
 *	How much data can be sent across a secure connection
 *	(total in both directions) before we require renegotiation.
 */
#define RENEGOTIATION_LIMIT (512 * 1024 * 1024)
#define CA_PATH NULL
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
 *	EDH even if the DBA... or an attacker... deletes the
 *	$DataDir/dh*.pem files.
 *
 *	We could refuse SSL connections unless a good DH parameter
 *	file exists, but some clients may quietly renegotiate an
 *	unsecured connection without fully informing the user.
 *	Very uncool.
 *
 *	Alternately, the backend could attempt to load these files
 *	on startup if SSL is enabled - and refuse to start if any
 *	do not exist - but this would tend to piss off DBAs.
 *
 *	If you want to create your own hardcoded DH parameters
 *	for fun and profit, review "Assigned Number for SKIP
 *	Protocols" (http://www.skip-vpn.org/spec/numbers.html)
 *	for suggestions.
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
secure_initialize(void)
{
	int			r = 0;

#ifdef USE_SSL
	r = initialize_SSL();
#endif

	return r;
}

/*
 *	Destroy global context
 */
void
secure_destroy(void)
{
#ifdef USE_SSL
	destroy_SSL();
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
	r = open_server_SSL(port);
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
	if (port->ssl)
		close_SSL(port);
#endif
}

/*
 *	Read data from a secure connection.
 */
ssize_t
secure_read(Port *port, void *ptr, size_t len)
{
	ssize_t		n;

#ifdef USE_SSL
	if (port->ssl)
	{
	rloop:
		n = SSL_read(port->ssl, ptr, len);
		switch (SSL_get_error(port->ssl, n))
		{
			case SSL_ERROR_NONE:
				port->count += n;
				break;
			case SSL_ERROR_WANT_READ:
			case SSL_ERROR_WANT_WRITE:
				goto rloop;
			case SSL_ERROR_SYSCALL:
				if (n == -1)
					elog(COMMERROR, "SSL SYSCALL error: %m");
				else
					elog(COMMERROR, "SSL SYSCALL error: EOF detected");
				break;
			case SSL_ERROR_SSL:
				elog(COMMERROR, "SSL error: %s", SSLerrmessage());
				/* fall through */
			case SSL_ERROR_ZERO_RETURN:
				secure_close(port);
				errno = ECONNRESET;
				n = -1;
				break;
			default:
				elog(COMMERROR, "Unknown SSL error code");
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
secure_write(Port *port, void *ptr, size_t len)
{
	ssize_t		n;

#ifdef USE_SSL
	if (port->ssl)
	{
		if (port->count > RENEGOTIATION_LIMIT)
		{
			SSL_set_session_id_context(port->ssl, (void *) &SSL_context,
									   sizeof(SSL_context));
			if (SSL_renegotiate(port->ssl) <= 0)
				elog(COMMERROR, "SSL renegotiation failure");
			if (SSL_do_handshake(port->ssl) <= 0)
				elog(COMMERROR, "SSL renegotiation failure");
			if (port->ssl->state != SSL_ST_OK)
				elog(COMMERROR, "SSL failed to send renegotiation request");
			port->ssl->state |= SSL_ST_ACCEPT;
			SSL_do_handshake(port->ssl);
			if (port->ssl->state != SSL_ST_OK)
				elog(COMMERROR, "SSL renegotiation failure");
			port->count = 0;
		}

	wloop:
		n = SSL_write(port->ssl, ptr, len);
		switch (SSL_get_error(port->ssl, n))
		{
			case SSL_ERROR_NONE:
				port->count += n;
				break;
			case SSL_ERROR_WANT_READ:
			case SSL_ERROR_WANT_WRITE:
				goto wloop;
			case SSL_ERROR_SYSCALL:
				if (n == -1)
					elog(COMMERROR, "SSL SYSCALL error: %m");
				else
					elog(COMMERROR, "SSL SYSCALL error: EOF detected");
				break;
			case SSL_ERROR_SSL:
				elog(COMMERROR, "SSL error: %s", SSLerrmessage());
				/* fall through */
			case SSL_ERROR_ZERO_RETURN:
				secure_close(port);
				errno = ECONNRESET;
				n = -1;
				break;
			default:
				elog(COMMERROR, "Unknown SSL error code");
				break;
		}
	}
	else
#endif
		n = send(port->sock, ptr, len, 0);

	return n;
}

/* ------------------------------------------------------------ */
/*						  SSL specific code						*/
/* ------------------------------------------------------------ */
#ifdef USE_SSL
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
	FILE	   *fp;
	char		fnbuf[2048];
	DH		   *dh = NULL;
	int			codes;

	/* attempt to open file.  It's not an error if it doesn't exist. */
	snprintf(fnbuf, sizeof fnbuf, "%s/dh%d.pem", DataDir, keylength);
	if ((fp = fopen(fnbuf, "r")) == NULL)
		return NULL;

/*	flock(fileno(fp), LOCK_SH); */
	dh = PEM_read_DHparams(fp, NULL, NULL, NULL);
/*	flock(fileno(fp), LOCK_UN); */
	fclose(fp);

	/* is the prime the correct size? */
	if (dh != NULL && 8 * DH_size(dh) < keylength)
	{
		elog(LOG, "DH errors (%s): %d bits expected, %d bits found",
			 fnbuf, keylength, 8 * DH_size(dh));
		dh = NULL;
	}

	/* make sure the DH parameters are usable */
	if (dh != NULL)
	{
		if (DH_check(dh, &codes))
		{
			elog(LOG, "DH_check error (%s): %s", fnbuf, SSLerrmessage());
			return NULL;
		}
		if (codes & DH_CHECK_P_NOT_PRIME)
		{
			elog(LOG, "DH error (%s): p is not prime", fnbuf);
			return NULL;
		}
		if ((codes & DH_NOT_SUITABLE_GENERATOR) &&
			(codes & DH_CHECK_P_NOT_SAFE_PRIME))
		{
			elog(LOG,
			   "DH error (%s): neither suitable generator or safe prime",
				 fnbuf);
			return NULL;
		}
	}

	return dh;
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
	if (dh == NULL)
		elog(DEBUG2, "DH load buffer: %s", SSLerrmessage());
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
	{
		elog(DEBUG2, "DH: generating parameters (%d bits)....", keylength);
		r = DH_generate_parameters(keylength, DH_GENERATOR_2, NULL, NULL);
	}

	return r;
}

/*
 *	Certificate verification callback
 *
 *	This callback allows us to log intermediate problems during
 *	verification, but for now we'll see if the final error message
 *	contains enough information.
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
 *	This callback is used to copy SSL information messages
 *	into the PostgreSQL log.
 */
static void
info_cb(const SSL *ssl, int type, int args)
{
	switch (type)
	{
		case SSL_CB_HANDSHAKE_START:
			elog(DEBUG4, "SSL: handshake start");
			break;
		case SSL_CB_HANDSHAKE_DONE:
			elog(DEBUG4, "SSL: handshake done");
			break;
		case SSL_CB_ACCEPT_LOOP:
			elog(DEBUG4, "SSL: accept loop");
			break;
		case SSL_CB_ACCEPT_EXIT:
			elog(DEBUG4, "SSL: accept exit (%d)", args);
			break;
		case SSL_CB_CONNECT_LOOP:
			elog(DEBUG4, "SSL: connect loop");
			break;
		case SSL_CB_CONNECT_EXIT:
			elog(DEBUG4, "SSL: connect exit (%d)", args);
			break;
		case SSL_CB_READ_ALERT:
			elog(DEBUG4, "SSL: read alert (0x%04x)", args);
			break;
		case SSL_CB_WRITE_ALERT:
			elog(DEBUG4, "SSL: write alert (0x%04x)", args);
			break;
	}
}

/*
 *	Initialize global SSL context.
 */
static int
initialize_SSL(void)
{
	char		fnbuf[2048];
	struct stat buf;

	if (!SSL_context)
	{
		SSL_library_init();
		SSL_load_error_strings();
		SSL_context = SSL_CTX_new(SSLv23_method());
		if (!SSL_context)
		{
			postmaster_error("failed to create SSL context: %s",
							 SSLerrmessage());
			ExitPostmaster(1);
		}

		/*
		 * Load and verify certificate and private key
		 */
		snprintf(fnbuf, sizeof(fnbuf), "%s/server.crt", DataDir);
		if (!SSL_CTX_use_certificate_file(SSL_context, fnbuf, SSL_FILETYPE_PEM))
		{
			postmaster_error("failed to load server certificate (%s): %s",
							 fnbuf, SSLerrmessage());
			ExitPostmaster(1);
		}

		snprintf(fnbuf, sizeof(fnbuf), "%s/server.key", DataDir);
		if (lstat(fnbuf, &buf) == -1)
		{
			postmaster_error("failed to stat private key file (%s): %s",
							 fnbuf, strerror(errno));
			ExitPostmaster(1);
		}
		if (!S_ISREG(buf.st_mode) || (buf.st_mode & 0077) ||
			buf.st_uid != getuid())
		{
			postmaster_error("bad permissions on private key file (%s)\n"
"File must be owned by the proper user and must have no permissions for\n"
"\"group\" or \"other\".", fnbuf);
			ExitPostmaster(1);
		}
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

	/* set up empheral DH keys */
	SSL_CTX_set_tmp_dh_callback(SSL_context, tmp_dh_cb);
	SSL_CTX_set_options(SSL_context, SSL_OP_SINGLE_DH_USE | SSL_OP_NO_SSLv2);

	/* setup the allowed cipher list */
	if (SSL_CTX_set_cipher_list(SSL_context, "ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH") != 1)
	{
		postmaster_error("unable to set the cipher list (no valid ciphers available)");
		ExitPostmaster(1);
	}

	/* accept client certificates, but don't require them. */
	snprintf(fnbuf, sizeof fnbuf, "%s/root.crt", DataDir);
	if (!SSL_CTX_load_verify_locations(SSL_context, fnbuf, CA_PATH))
	{
		return 0;
#ifdef NOT_USED
		/* CLIENT CERTIFICATES NOT REQUIRED  bjm 2002-09-26 */
		postmaster_error("could not read root cert file (%s): %s",
						 fnbuf, SSLerrmessage());
		ExitPostmaster(1);
#endif
	}
	SSL_CTX_set_verify(SSL_context,
					SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE, verify_cb);

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
static int
open_server_SSL(Port *port)
{
	if (!(port->ssl = SSL_new(SSL_context)) ||
		!SSL_set_fd(port->ssl, port->sock) ||
		SSL_accept(port->ssl) <= 0)
	{
		elog(COMMERROR, "failed to initialize SSL connection: %s", SSLerrmessage());
		close_SSL(port);
		return -1;
	}
	port->count = 0;

	/* get client certificate, if available. */
	port->peer = SSL_get_peer_certificate(port->ssl);
	if (port->peer == NULL)
	{
		strncpy(port->peer_dn, "(anonymous)", sizeof(port->peer_dn));
		strncpy(port->peer_cn, "(anonymous)", sizeof(port->peer_cn));
	}
	else
	{
		X509_NAME_oneline(X509_get_subject_name(port->peer),
						  port->peer_dn, sizeof(port->peer_dn));
		port->peer_dn[sizeof(port->peer_dn) - 1] = '\0';
		X509_NAME_get_text_by_NID(X509_get_subject_name(port->peer),
				   NID_commonName, port->peer_cn, sizeof(port->peer_cn));
		port->peer_cn[sizeof(port->peer_cn) - 1] = '\0';
	}
	elog(DEBUG2, "secure connection from '%s'", port->peer_cn);

	/* set up debugging/info callback */
	SSL_CTX_set_info_callback(SSL_context, info_cb);

	return 0;
}

/*
 *	Close SSL connection.
 */
static void
close_SSL(Port *port)
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

#endif   /* USE_SSL */
