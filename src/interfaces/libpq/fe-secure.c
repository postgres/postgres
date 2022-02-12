/*-------------------------------------------------------------------------
 *
 * fe-secure.c
 *	  functions related to setting up a secure connection to the backend.
 *	  Secure connections are expected to provide confidentiality,
 *	  message integrity and endpoint authentication.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-secure.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <signal.h>
#include <fcntl.h>
#include <ctype.h>

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

#include "fe-auth.h"
#include "libpq-fe.h"
#include "libpq-int.h"

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
#endif							/* ENABLE_THREAD_SAFETY */
#else							/* WIN32 */

#define DECLARE_SIGPIPE_INFO(spinfo)
#define DISABLE_SIGPIPE(conn, spinfo, failaction)
#define REMEMBER_EPIPE(spinfo, cond)
#define RESTORE_SIGPIPE(conn, spinfo)
#endif							/* WIN32 */

/* ------------------------------------------------------------ */
/*			 Procedures common to all secure sessions			*/
/* ------------------------------------------------------------ */


int
PQsslInUse(PGconn *conn)
{
	if (!conn)
		return 0;
	return conn->ssl_in_use;
}

/*
 *	Exported function to allow application to tell us it's already
 *	initialized OpenSSL.
 */
void
PQinitSSL(int do_init)
{
#ifdef USE_SSL
	pgtls_init_library(do_init, do_init);
#endif
}

/*
 *	Exported function to allow application to tell us it's already
 *	initialized OpenSSL and/or libcrypto.
 */
void
PQinitOpenSSL(int do_ssl, int do_crypto)
{
#ifdef USE_SSL
	pgtls_init_library(do_ssl, do_crypto);
#endif
}

/*
 *	Initialize global SSL context
 */
int
pqsecure_initialize(PGconn *conn, bool do_ssl, bool do_crypto)
{
	int			r = 0;

#ifdef USE_SSL
	r = pgtls_init(conn, do_ssl, do_crypto);
#endif

	return r;
}

/*
 *	Begin or continue negotiating a secure session.
 */
PostgresPollingStatusType
pqsecure_open_client(PGconn *conn)
{
#ifdef USE_SSL
	return pgtls_open_client(conn);
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
	pgtls_close(conn);
#endif
}

/*
 *	Read data from a secure connection.
 *
 * On failure, this function is responsible for appending a suitable message
 * to conn->errorMessage.  The caller must still inspect errno, but only
 * to determine whether to continue/retry after error.
 */
ssize_t
pqsecure_read(PGconn *conn, void *ptr, size_t len)
{
	ssize_t		n;

#ifdef USE_SSL
	if (conn->ssl_in_use)
	{
		n = pgtls_read(conn, ptr, len);
	}
	else
#endif
#ifdef ENABLE_GSS
	if (conn->gssenc)
	{
		n = pg_GSS_read(conn, ptr, len);
	}
	else
#endif
	{
		n = pqsecure_raw_read(conn, ptr, len);
	}

	return n;
}

ssize_t
pqsecure_raw_read(PGconn *conn, void *ptr, size_t len)
{
	ssize_t		n;
	int			result_errno = 0;
	char		sebuf[PG_STRERROR_R_BUFLEN];

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

			case EPIPE:
			case ECONNRESET:
				appendPQExpBufferStr(&conn->errorMessage,
									 libpq_gettext("server closed the connection unexpectedly\n"
												   "\tThis probably means the server terminated abnormally\n"
												   "\tbefore or while processing the request.\n"));
				break;

			default:
				appendPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("could not receive data from server: %s\n"),
								  SOCK_STRERROR(result_errno,
												sebuf, sizeof(sebuf)));
				break;
		}
	}

	/* ensure we return the intended errno to caller */
	SOCK_ERRNO_SET(result_errno);

	return n;
}

/*
 *	Write data to a secure connection.
 *
 * Returns the number of bytes written, or a negative value (with errno
 * set) upon failure.  The write count could be less than requested.
 *
 * Note that socket-level hard failures are masked from the caller,
 * instead setting conn->write_failed and storing an error message
 * in conn->write_err_msg; see pqsecure_raw_write.  This allows us to
 * postpone reporting of write failures until we're sure no error
 * message is available from the server.
 *
 * However, errors detected in the SSL or GSS management level are reported
 * via a negative result, with message appended to conn->errorMessage.
 * It's frequently unclear whether such errors should be considered read or
 * write errors, so we don't attempt to postpone reporting them.
 *
 * The caller must still inspect errno upon failure, but only to determine
 * whether to continue/retry; a message has been saved someplace in any case.
 */
ssize_t
pqsecure_write(PGconn *conn, const void *ptr, size_t len)
{
	ssize_t		n;

#ifdef USE_SSL
	if (conn->ssl_in_use)
	{
		n = pgtls_write(conn, ptr, len);
	}
	else
#endif
#ifdef ENABLE_GSS
	if (conn->gssenc)
	{
		n = pg_GSS_write(conn, ptr, len);
	}
	else
#endif
	{
		n = pqsecure_raw_write(conn, ptr, len);
	}

	return n;
}

/*
 * Low-level implementation of pqsecure_write.
 *
 * This is used directly for an unencrypted connection.  For encrypted
 * connections, this does the physical I/O on behalf of pgtls_write or
 * pg_GSS_write.
 *
 * This function reports failure (i.e., returns a negative result) only
 * for retryable errors such as EINTR.  Looping for such cases is to be
 * handled at some outer level, maybe all the way up to the application.
 * For hard failures, we set conn->write_failed and store an error message
 * in conn->write_err_msg, but then claim to have written the data anyway.
 * This is because we don't want to report write failures so long as there
 * is a possibility of reading from the server and getting an error message
 * that could explain why the connection dropped.  Many TCP stacks have
 * race conditions such that a write failure may or may not be reported
 * before all incoming data has been read.
 *
 * Note that this error behavior happens below the SSL management level when
 * we are using SSL.  That's because at least some versions of OpenSSL are
 * too quick to report a write failure when there's still a possibility to
 * get a more useful error from the server.
 */
ssize_t
pqsecure_raw_write(PGconn *conn, const void *ptr, size_t len)
{
	ssize_t		n;
	int			flags = 0;
	int			result_errno = 0;
	char		msgbuf[1024];
	char		sebuf[PG_STRERROR_R_BUFLEN];

	DECLARE_SIGPIPE_INFO(spinfo);

	/*
	 * If we already had a write failure, we will never again try to send data
	 * on that connection.  Even if the kernel would let us, we've probably
	 * lost message boundary sync with the server.  conn->write_failed
	 * therefore persists until the connection is reset, and we just discard
	 * all data presented to be written.
	 */
	if (conn->write_failed)
		return len;

#ifdef MSG_NOSIGNAL
	if (conn->sigpipe_flag)
		flags |= MSG_NOSIGNAL;

retry_masked:
#endif							/* MSG_NOSIGNAL */

	DISABLE_SIGPIPE(conn, spinfo, return -1);

	n = send(conn->sock, ptr, len, flags);

	if (n < 0)
	{
		result_errno = SOCK_ERRNO;

		/*
		 * If we see an EINVAL, it may be because MSG_NOSIGNAL isn't available
		 * on this machine.  So, clear sigpipe_flag so we don't try the flag
		 * again, and retry the send().
		 */
#ifdef MSG_NOSIGNAL
		if (flags != 0 && result_errno == EINVAL)
		{
			conn->sigpipe_flag = false;
			flags = 0;
			goto retry_masked;
		}
#endif							/* MSG_NOSIGNAL */

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

			case ECONNRESET:
				conn->write_failed = true;
				/* Store error message in conn->write_err_msg, if possible */
				/* (strdup failure is OK, we'll cope later) */
				snprintf(msgbuf, sizeof(msgbuf),
						 libpq_gettext("server closed the connection unexpectedly\n"
									   "\tThis probably means the server terminated abnormally\n"
									   "\tbefore or while processing the request.\n"));
				conn->write_err_msg = strdup(msgbuf);
				/* Now claim the write succeeded */
				n = len;
				break;

			default:
				conn->write_failed = true;
				/* Store error message in conn->write_err_msg, if possible */
				/* (strdup failure is OK, we'll cope later) */
				snprintf(msgbuf, sizeof(msgbuf),
						 libpq_gettext("could not send data to server: %s\n"),
						 SOCK_STRERROR(result_errno,
									   sebuf, sizeof(sebuf)));
				conn->write_err_msg = strdup(msgbuf);
				/* Now claim the write succeeded */
				n = len;
				break;
		}
	}

	RESTORE_SIGPIPE(conn, spinfo);

	/* ensure we return the intended errno to caller */
	SOCK_ERRNO_SET(result_errno);

	return n;
}

/* Dummy versions of SSL info functions, when built without SSL support */
#ifndef USE_SSL

void *
PQgetssl(PGconn *conn)
{
	return NULL;
}

void *
PQsslStruct(PGconn *conn, const char *struct_name)
{
	return NULL;
}

const char *
PQsslAttribute(PGconn *conn, const char *attribute_name)
{
	return NULL;
}

const char *const *
PQsslAttributeNames(PGconn *conn)
{
	static const char *const result[] = {NULL};

	return result;
}
#endif							/* USE_SSL */

/*
 * Dummy versions of OpenSSL key password hook functions, when built without
 * OpenSSL.
 */
#ifndef USE_OPENSSL

PQsslKeyPassHook_OpenSSL_type
PQgetSSLKeyPassHook_OpenSSL(void)
{
	return NULL;
}

void
PQsetSSLKeyPassHook_OpenSSL(PQsslKeyPassHook_OpenSSL_type hook)
{
	return;
}

int
PQdefaultSSLKeyPassHook_OpenSSL(char *buf, int size, PGconn *conn)
{
	return 0;
}
#endif							/* USE_OPENSSL */

/* Dummy version of GSSAPI information functions, when built without GSS support */
#ifndef ENABLE_GSS

void *
PQgetgssctx(PGconn *conn)
{
	return NULL;
}

int
PQgssEncInUse(PGconn *conn)
{
	return 0;
}

#endif							/* ENABLE_GSS */


#if defined(ENABLE_THREAD_SAFETY) && !defined(WIN32)

/*
 *	Block SIGPIPE for this thread.  This prevents send()/write() from exiting
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
 * up multiple SIGPIPE events.  If it did, then we'd accidentally leave
 * ours in the queue when an event was already pending and we got another.
 * As long as it doesn't queue multiple events, we're OK because the caller
 * can't tell the difference.
 *
 * The caller should say got_epipe = false if it is certain that it
 * didn't get an EPIPE error; in that case we'll skip the clear operation
 * and things are definitely OK, queuing or no.  If it got one or might have
 * gotten one, pass got_epipe = true.
 *
 * We do not want this to change errno, since if it did that could lose
 * the error code from a preceding send().  We essentially assume that if
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

#endif							/* ENABLE_THREAD_SAFETY && !WIN32 */
