#include "postgres_fe.h"

#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <pwd.h>

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

#ifdef USE_SSL
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/e_os.h>

int initialize_ctx(const char *, void (*err)(const char *fmt,...), PGconn *);
void destroy_ctx(void);
int open_SSL_client(PGconn *);
void close_SSL(PGconn *);
SSL PGgetssl(PGconn *);
static int clientCert_cb(SSL *ssl, X509 **x509, EVP_PKEY **pkey);
static int verify_cb(int, X509_STORE_CTX *);
static void info_cb(SSL *ssl, int type, int args);
static void load_hardcoded_certs(void);
static X509 * load_cert_buffer(const char *buf, size_t len);
static const char *SSLerrmessage(void);
#endif

ssize_t read_SSL(PGconn *, void *, size_t);
ssize_t write_SSL(PGconn *, const void *, size_t);

extern int h_error;

#ifdef USE_SSL
static SSL_CTX *ctx = NULL;
#endif

#define PING() fprintf(stderr,"%s, line %d, %s\n", __FILE__, __LINE__, __func__)

/*
 *	Read data from network.
 */
ssize_t read_SSL (PGconn *conn, void *ptr, size_t len)
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
			break;
		case SSL_ERROR_SSL:
//			log error...
			SOCK_ERRNO = ECONNRESET;
			break;
		case SSL_ERROR_ZERO_RETURN:
			SOCK_ERRNO = ECONNRESET;
			break;
		}
	}
	else
#endif /* USE_SSL */
	n = recv(conn->sock, ptr, len, 0);

	return n;
}

/*
 *	Write data to network.
 */
ssize_t write_SSL (PGconn *conn, const void *ptr, size_t len)
{
	ssize_t n;

	/* prevent being SIGPIPEd if backend has closed the connection. */
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
			break;
		case SSL_ERROR_SSL:
fprintf(stderr, "ssl error\n");
//			log error...
			SOCK_ERRNO = ECONNRESET;
			break;
		case SSL_ERROR_ZERO_RETURN:
fprintf(stderr, "zero bytes\n");
			SOCK_ERRNO = ECONNRESET;
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

#ifdef USE_SSL
/*
 *	Null authentication callback
 */
static int
verify_cb (int ok, X509_STORE_CTX *ctx)
{
	char sn[256], buf[256];
	X509 *cert;
	int err, depth, n;
	BIO *bio;

	cert = X509_STORE_CTX_get_current_cert(ctx);
	err  = X509_STORE_CTX_get_error(ctx);
	depth= X509_STORE_CTX_get_error_depth(ctx);

	X509_NAME_oneline(X509_get_subject_name(cert), sn, sizeof sn);
	if (!ok)
	{
		switch (err)
		{
		/* accept self-signed certs */
//		case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
//			ok = 1;
//			break;

		default:
			fprintf(stderr, "client cert %s: %s", sn,
				X509_verify_cert_error_string(err));
		}
	}

	switch (ctx->error)
	{
	case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
		X509_NAME_oneline(X509_get_issuer_name(cert), buf, sizeof buf);
		fprintf(stderr, "client cert %s: cannot find issuer %s", sn, buf);
		break;
	case X509_V_ERR_CERT_NOT_YET_VALID:
	case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
		bio = BIO_new(BIO_s_mem());
		ASN1_TIME_print(bio, X509_get_notBefore(cert));
		BIO_flush(bio);
		n = BIO_read(bio, buf, sizeof buf - 1);
		buf[n] = '\0';
		fprintf(stderr, "client cert %s: not valid until %s", sn, buf);
		break;
	case X509_V_ERR_CERT_HAS_EXPIRED:
	case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
		bio = BIO_new(BIO_s_mem());
		ASN1_TIME_print(bio, X509_get_notAfter(cert));
		BIO_flush(bio);
		n = BIO_read(bio, buf, sizeof buf - 1);
		buf[n] = '\0';
		fprintf(stderr, "client cert %s: not valid after %s\n", sn, buf);
		break;
	}

	return ok;
}

/*
 *	Callback used by SSL to provide information messages.
 */
static void
info_cb (SSL *ssl, int type, int args)
{
	PGconn *conn = NULL;

	conn = (PGconn *) SSL_get_app_data(ssl);
	if (conn == NULL || conn->Pfdebug == NULL)
		return;

	switch (type)
	{
	case SSL_CB_HANDSHAKE_START:
		fprintf(conn->Pfdebug, "Handshake start\n");
		break;
	case SSL_CB_HANDSHAKE_DONE:
		fprintf(conn->Pfdebug, "Handshake done\n");
		break;
	case SSL_CB_ACCEPT_LOOP:
		fprintf(conn->Pfdebug, "Accept loop...\n");
		break;
	case SSL_CB_ACCEPT_EXIT:
		fprintf(conn->Pfdebug, "Accept exit (%d)\n", args);
		break;
	case SSL_CB_CONNECT_LOOP:
		fprintf(conn->Pfdebug, "Connect loop...\n");
		break;
	case SSL_CB_CONNECT_EXIT:
		fprintf(conn->Pfdebug, "Connect exit (%d)\n", args);
		break;
	case SSL_CB_READ_ALERT:
		fprintf(conn->Pfdebug, "Read alert (0x%04x)\n", args);
		break;
	case SSL_CB_WRITE_ALERT:
		fprintf(conn->Pfdebug, "Write alert (0x%04x)\n", args);
		break;
	}
}

/*
 *	Callback used by SSL to load client cert and key.
 *	At the current time we require the cert and key to be
 *	located in the .postgresql directory under the user's
 *	home directory, and the files must be named 'postgresql.crt'
 *	and 'postgresql.key' respectively.
 * 
 *	returns 1 on success, 0 on no data, -1 on error.
 */
static int
clientCert_cb (SSL *ssl, X509 **x509, EVP_PKEY **pkey)
{
	uid_t uid;
	struct passwd *pwd;
	char fnbuf[2048];
	struct stat buf, buf1;
	FILE *fp;
	int (*cb)() = NULL;

	if ((uid = getuid()) == -1)
	{
		fprintf(stderr, "can't get current uid\n");
		return -1;
	}
	if ((pwd = getpwuid(uid)) == NULL || !pwd->pw_dir)
	{
		fprintf(stderr, "can't get passwd entry\n");
		return -1;
	}

	/*
	 * if $HOME/.postgresql does not exist, 'no data' case.
	 * otherwise, it must be a directory, owned by current user,
	 * and not group- or world-accessible. 
	 */
	snprintf(fnbuf, sizeof fnbuf,  "%s/.postgresql", pwd->pw_dir);
	if (lstat(fnbuf, &buf) == -1)
		return 0;
	if (!S_ISDIR(buf.st_mode) || buf.st_uid != uid ||
		(buf.st_mode & (S_IRWXG | S_IRWXO)) != 0)
	{
		fprintf(stderr,
			"$HOME/.postgresql directory has wrong ownership or permissions\n");
		return -1;
	}

	/*
	 * make sure $HOME/.postgresql/postgresql.crt file exists,
	 * is regular file and owned by current user.
	 */
	snprintf(fnbuf, sizeof fnbuf,  "%s/.postgresql/postgresql.crt", 
		pwd->pw_dir);
	if (lstat(fnbuf, &buf) == -1)
		return 0;
	if (!S_ISREG(buf.st_mode) || buf.st_uid != uid)
	{
		fprintf(stderr,
			"certificate file has wrong ownership or permissions\n");
		return -1;
	}
	if ((fp = fopen(fnbuf, "r")) == NULL)
	{
		fprintf(stderr, "can't open certificate file (%s)\n", strerror(errno));
		return -1;
	}
	if (PEM_read_X509(fp, x509, NULL, NULL) == NULL)
	{
		fprintf(stderr, "can't read certificate %s\n", SSLerrmessage());
		fclose(fp);
		return -1;
	}
	fclose(fp);

	/*
	 * make sure $HOME/.postgresql/postgresql.key file exists,
	 * is regular file, owned by current user, and not group-
	 * or world-accessable.
	 */
	snprintf(fnbuf, sizeof fnbuf,  "%s/.postgresql/postgresql.key", 
		pwd->pw_dir);
	if (lstat(fnbuf, &buf) == -1)
	{
		fprintf(stderr, "certificate file exists, but no private key\n");
		SSL_use_certificate(ssl, NULL);
		return -1;
	}
	if (!S_ISREG(buf.st_mode) || buf.st_uid != uid ||
		(buf.st_mode & (S_IRWXG | S_IRWXO)) != 0)
	{
		fprintf(stderr,
			"private key file has wrong ownership or permissions\n");
		SSL_use_certificate(ssl, NULL);
		return -1;
	}
	if ((fp = fopen(fnbuf, "r")) == NULL)
	{
		fprintf(stderr, "error opening private key file: %s\n",
			strerror(errno));
		SSL_use_certificate(ssl, NULL);
		return -1;
	}
	if (fstat(fileno(fp),&buf1) == -1 ||
		buf.st_dev != buf1.st_dev || buf.st_ino != buf1.st_ino)
	{
		fprintf(stderr, "private key changed under us!\n");
		fclose(fp);
		SSL_use_certificate(ssl, NULL);
		return -1;
	}
	if (PEM_read_PrivateKey(fp, pkey, cb, NULL) == NULL)
	{
		fprintf(stderr, "can't read private key %s\n", SSLerrmessage());
		fclose(fp);
		SSL_use_certificate(ssl, NULL);
		return -1;
	}
	fclose(fp);

	return 1;
}

/*
 *	Load a root cert from a buffer.  This allows us to avoid
 *	needing to copy the root cert to deployed systems.
 */
static X509 *
load_cert_buffer(const char *buf, size_t len)
{
	BIO *bio;
	X509 *x;

	bio = BIO_new_mem_buf((char *) buf, len);
	x = PEM_read_bio_X509(bio, NULL, NULL, NULL);
	BIO_free(bio);

	return x;
}

/*
 *	Initialize global SSL context.
 *
 * 	We want to use 'err' for errors, same as the corresponding 
 *	function on the server, but for now we use legacy error handler
 *	in PGconn.
 */
int 
initialize_ctx (const char *password,
	void (*err)(const char * fmt,...), PGconn *conn)
{
	SSL_METHOD *meth = NULL;
	struct stat buf;
	struct passwd *pwd;
	char fnbuf[2048];

	if (!ctx)
	{
		SSL_library_init();
		SSL_load_error_strings();
//		meth = SSLv23_method();
		meth = TLSv1_method();
		ctx = SSL_CTX_new(meth);

		if (!ctx) {
			printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("could not create SSL context: %s\n"),
					SSLerrmessage());
			return -1;
		}
	}

	/* load any hard-coded root cert */
	load_hardcoded_certs();

	/* load the CAs we trust */
	if ((pwd = getpwuid(getuid())) != NULL)
	{
		snprintf(fnbuf, sizeof fnbuf, "%s/.postgresql/root.crt", pwd->pw_dir);
		if (stat(fnbuf, &buf) != -1)
		{
			if (!SSL_CTX_load_verify_locations(ctx, fnbuf, 0))
			{
			printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("could not read CA list (%s): %s\n"),
					fnbuf, SSLerrmessage());
			return -1;
			}
		}
	}

	/* load randomness */
#ifdef RANDOM
	if (!RAND_load_file(RANDOM, 1024 * 1024))
	{
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("could not load randomness (%s): %s\n"),
				RANDOM, SSLerrmessage());
		return -1;
	}
#else /* RANDOM */
	if (lstat("/dev/urandom", &buf) == 0 && S_ISCHR(buf.st_mode))
	{
		if (!RAND_load_file("/dev/urandom", 16 * 1024))
		{
			printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("could not load randomness (%s): %s\n"),
					"/dev/urandom", SSLerrmessage());
			return -1;
		}
	}
#endif /* RANDOM */

	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, verify_cb);
	SSL_CTX_set_verify_depth(ctx, 1);

	SSL_CTX_set_info_callback(ctx, info_cb);
	SSL_CTX_set_client_cert_cb(ctx, clientCert_cb);

	return 0;
}

/*
 *	Destroy the global SSL context.
 */
void destroy_ctx (void)
{
	SSL_CTX_free(ctx);
	ctx = NULL;
}

/*
 *	Open a SSL connection.
 */
int
open_SSL_client (PGconn *conn)
{
	char peerName[256];
	struct sockaddr addr;
	struct sockaddr_in *sin1, *sin2;
	socklen_t len;
	struct hostent *h;
	const char *reason;
	char **s;
	int r;

	if (!(conn->ssl = SSL_new(ctx)) ||
		!SSL_set_app_data(conn->ssl, conn) ||
		!SSL_set_fd(conn->ssl, conn->sock) ||
		SSL_connect(conn->ssl) <= 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("could not establish SSL connection: %s\n"),
				SSLerrmessage());
		return -1;
	}

	/* check the certificate chain */
	/* for now, we allow self-signed server certs */
	r = SSL_get_verify_result(conn->ssl);
//	if (r != X509_V_OK && r != X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT)
	if (r != X509_V_OK)
	{
		switch (r)
		{
		case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
			reason = "unable to get issuer cert";
			break;	
		case X509_V_ERR_UNABLE_TO_GET_CRL:
			reason = "unable to get CRL";
			break;	
		case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:
			reason = "unable to decrypt cert signature";
			break;	
		case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE:
			reason = "unable to decrypt CRL signature";
			break;	
		case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
			reason = "unable to decode issuer public key";
			break;	
		case X509_V_ERR_CERT_SIGNATURE_FAILURE:
			reason = "cert signature failure";
			break;	
		case X509_V_ERR_CRL_SIGNATURE_FAILURE:
			reason = "CRL signature failure";
			break;	
		case X509_V_ERR_CERT_NOT_YET_VALID:
			reason = "cert is not yet valid";
			break;	
		case X509_V_ERR_CERT_HAS_EXPIRED:
			reason = "cert has expired";
			break;	
		case X509_V_ERR_CRL_NOT_YET_VALID:
			reason = "CRL not yet valid";
			break;	
		case X509_V_ERR_CRL_HAS_EXPIRED:
			reason = "CRL has expired";
			break;	
		case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
			reason = "error in cert notBefore field";
			break;	
		case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
			reason = "error in cert notAfter field";
			break;	
		case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD:
			reason = "error in CRL last update field";
			break;	
		case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD:
			reason = "error in CRL next update field";
			break;	
		case X509_V_ERR_OUT_OF_MEM:
			reason = "out of memory";
			break;	
		case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
			reason = "depth zero self-signed cert";
			break;	
		case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
			reason = "self-signed cert in chain";
			break;	
		case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
			reason = "unable to get issuer cert locally";
			break;	
		case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
			reason = "unable to verify leaf signature";
			break;	
		case X509_V_ERR_CERT_CHAIN_TOO_LONG:
			reason = "cert chain too long";
			break;	
		case X509_V_ERR_CERT_REVOKED:
			reason = "cert revoked";
			break;	
		case X509_V_ERR_INVALID_CA:
			reason = "invalid CA";
			break;	
		case X509_V_ERR_PATH_LENGTH_EXCEEDED:
			reason = "path length exceeded";
			break;	
		case X509_V_ERR_INVALID_PURPOSE:
			reason = "invalid purpose";
			break;	
		case X509_V_ERR_CERT_UNTRUSTED:
			reason = "cert untrusted";
			break;	
		case X509_V_ERR_CERT_REJECTED:
			reason = "cert rejected";
			break;	
		/* These are 'informational' when looking for issuer cert */
		case X509_V_ERR_SUBJECT_ISSUER_MISMATCH:
			reason = "cert issuer/issuer subject mismatch";
			break;	
		case X509_V_ERR_AKID_SKID_MISMATCH:
			reason = "cert akid/issuer skid mismatch";
			break;	
		case X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH:
			reason = "cert akid/issuer serial mismatch";
			break;	
		case X509_V_ERR_KEYUSAGE_NO_CERTSIGN:
			reason = "keyusage no certsign";
			break;	
		/* The application is not happy */
		case X509_V_ERR_APPLICATION_VERIFICATION:
			reason = "application-specific verification error";
			break;
		default:
			reason = "unknown reason";
		}
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("certificate could not be verified: %s (%d)\n"),
				reason, r);
		return -1;
	}

	/* do a reverse lookup on the server */
	len = sizeof(addr);
	if (getpeername(conn->sock, &addr, &len) == -1)
	{
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("error querying socket: %s\n"), strerror(errno));
		return -1;
	}
	if (addr.sa_family != AF_INET)
	{
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("not on IPv4 socket\n"));
		return -1;
	}

	/* check the cert common name */
	conn->peer = SSL_get_peer_certificate(conn->ssl);
	X509_NAME_get_text_by_NID(X509_get_subject_name(conn->peer),
		NID_commonName, peerName, sizeof peerName);
	if ((h = gethostbyname2(peerName, addr.sa_family)) == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("error looking up address %s: %s\n"),
				peerName, hstrerror(h_errno));
		return -1;
	}

	/* check for a match on actual socket address */
	sin1 = (struct sockaddr_in *) &addr;
	for (s = h->h_addr_list; *s != NULL; s++)
	{
		sin2 = (struct sockaddr_in *) *s;
		if (sin1->sin_addr.s_addr == sin2->sin_addr.s_addr)
			break;
	}

	/* if that failed, check for a match on alias */
	if (*s == NULL)
		{
		if (strcasecmp(peerName, conn->pghost) == 0)
			;
		else
		{
			for (s = h->h_aliases; *s != NULL; s++)
			{
				if (strcasecmp(peerName, *s) == 0)
					break;
			}
			if (*s == NULL)
			{
				printfPQExpBuffer(&conn->errorMessage,
					libpq_gettext(
						"certificate name (%s) does not match peer address\n"),
					peerName);
				return -1;
			}
		}
	}
		
	return 0;
}

/*
 *	Close a SSL connection.
 */
void
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
 *	Accessor function that retrieves SSL connection pointer.
 */
SSL *
PQgetssl (PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->ssl;
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
 *	The following conditional block shows how to embedded
 *	one or more root certs into the libpq library.  This
 *	eliminates any need to copy the file to the clients, but
 *	obviously must be done on a per-site basis.
 */
#if 0
/*
 *	The cert file, in PEM format, copied into a string buffer.
 */
static const char root1[] =
"-----BEGIN CERTIFICATE-----\n\
MIIEqDCCBGagAwIBAgIBADALBgcqhkjOOAQDBQAwgYwxEzARBgoJkiaJk/IsZAEZ\n\
EwNjb20xGjAYBgoJkiaJk/IsZAEZEwpjb3lvdGVzb25nMRIwEAYDVQQKEwlTbmFr\n\
ZSBPaWwxHTAbBgNVBAMTFFBvc3RncmVTUUwgUm9vdCBDZXJ0MSYwJAYJKoZIhvcN\n\
AQkBFhdwb3N0Z3Jlc0Bjb3lvdGVzb25nLmNvbTAeFw0wMjA1MjEwMDE4MDZaFw0w\n\
MjA2MjAwMDE4MDZaMIGMMRMwEQYKCZImiZPyLGQBGRMDY29tMRowGAYKCZImiZPy\n\
LGQBGRMKY295b3Rlc29uZzESMBAGA1UEChMJU25ha2UgT2lsMR0wGwYDVQQDExRQ\n\
b3N0Z3JlU1FMIFJvb3QgQ2VydDEmMCQGCSqGSIb3DQEJARYXcG9zdGdyZXNAY295\n\
b3Rlc29uZy5jb20wggG2MIIBKwYHKoZIzjgEATCCAR4CgYEAxgmwTdzv7eSqUjcS\n\
8fdT/3lm+On8LmHL+CkmF7IlvZKm2kwIiQqjcrG6JqgXBdBTIzeqSZV8cGrc0/f5\n\
zMh6rDVxuSrEwCh8DtAC9LdwWyHp7Tw79z9khkZNTAlBonwOLvm0BJaroH5FLK9S\n\
PvAHmjmLA1zd/2K8o+CqFFJasTkCFQDXfI1tnskPUtPXz/W88wRg5y5zpQKBgGwk\n\
3a+tfWmw2mMDXh2sSHoGwVlzwqKZnDfk97I7Tz/zmGOLEGdA7s+2YqKKfW7F0S8p\n\
Ho/cYDNE2lyaGqaxl2pscqdIhEmKYjJtjgaOOkQwfaYXs5GY0zkiSaxxtvJTj0WK\n\
OQ+J/0iunsyyukYc3+TiosHENz4Y2ZgaGseJTMz0A4GEAAKBgFG5WK5/64gjuJ7D\n\
D4RQ7QZtZ+wxP4s3oEqphz4hPGpGOPYlHdo2PhHMEAVrgMnX44yqUAnwmG5LT1RI\n\
5KPCDwgyxBQVq2FDJrYoRb/AVbqMQ8cyJZ1etd7J1ies31b3fHp+uYSFHuCmLfFp\n\
RO8wLplYM6XmJ5X5BF8zlclDxIj/o4IBVTCCAVEwHQYDVR0OBBYEFMO7rhIEVsrn\n\
6k/gxKR5bCdEo8jZMIG5BgNVHSMEgbEwga6AFMO7rhIEVsrn6k/gxKR5bCdEo8jZ\n\
oYGSpIGPMIGMMRMwEQYKCZImiZPyLGQBGRMDY29tMRowGAYKCZImiZPyLGQBGRMK\n\
Y295b3Rlc29uZzESMBAGA1UEChMJU25ha2UgT2lsMR0wGwYDVQQDExRQb3N0Z3Jl\n\
U1FMIFJvb3QgQ2VydDEmMCQGCSqGSIb3DQEJARYXcG9zdGdyZXNAY295b3Rlc29u\n\
Zy5jb22CAQAwDAYDVR0TBAUwAwEB/zALBgNVHQ8EBAMCAQYwEQYJYIZIAYb4QgEB\n\
BAQDAgEGMCIGA1UdEQQbMBmBF3Bvc3RncmVzQGNveW90ZXNvbmcuY29tMCIGA1Ud\n\
EgQbMBmBF3Bvc3RncmVzQGNveW90ZXNvbmcuY29tMAsGByqGSM44BAMFAAMvADAs\n\
AhUAhcafaeM39bK2z2tgRD8OLbrr3fICEwdVqUy9ykb9Hc7SjcKB51lUJ9s=\n\
-----END CERTIFICATE-----\n";

static void
load_hardcoded_certs(void)
{
	X509_STORE *store;
	X509 *x;

	store = SSL_CTX_get_cert_store(ctx);
	if (store != NULL)
	{
		x = load_cert_buffer(root1, sizeof (root1));
		X509_STORE_add_cert(store, x);
		X509_free(x);

		/* repeat as necessary.... */
	}
}
#else
static void
load_hardcoded_certs(void)
{
}
#endif

#endif /* USE_SSL */
