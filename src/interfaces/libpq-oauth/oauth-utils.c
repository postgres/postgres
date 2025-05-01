/*-------------------------------------------------------------------------
 *
 * oauth-utils.c
 *
 *	  "Glue" helpers providing a copy of some internal APIs from libpq. At
 *	  some point in the future, we might be able to deduplicate.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq-oauth/oauth-utils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <signal.h>

#include "oauth-utils.h"

#ifndef USE_DYNAMIC_OAUTH
#error oauth-utils.c is not supported in static builds
#endif

#ifdef LIBPQ_INT_H
#error do not rely on libpq-int.h in dynamic builds of libpq-oauth
#endif

/*
 * Function pointers set by libpq_oauth_init().
 */

pgthreadlock_t pg_g_threadlock;
static libpq_gettext_func libpq_gettext_impl;

conn_errorMessage_func conn_errorMessage;
conn_oauth_client_id_func conn_oauth_client_id;
conn_oauth_client_secret_func conn_oauth_client_secret;
conn_oauth_discovery_uri_func conn_oauth_discovery_uri;
conn_oauth_issuer_id_func conn_oauth_issuer_id;
conn_oauth_scope_func conn_oauth_scope;
conn_sasl_state_func conn_sasl_state;

set_conn_altsock_func set_conn_altsock;
set_conn_oauth_token_func set_conn_oauth_token;

/*-
 * Initializes libpq-oauth by setting necessary callbacks.
 *
 * The current implementation relies on the following private implementation
 * details of libpq:
 *
 * - pg_g_threadlock: protects libcurl initialization if the underlying Curl
 *   installation is not threadsafe
 *
 * - libpq_gettext: translates error messages using libpq's message domain
 *
 * The implementation also needs access to several members of the PGconn struct,
 * which are not guaranteed to stay in place across minor versions. Accessors
 * (named conn_*) and mutators (named set_conn_*) are injected here.
 */
void
libpq_oauth_init(pgthreadlock_t threadlock_impl,
				 libpq_gettext_func gettext_impl,
				 conn_errorMessage_func errmsg_impl,
				 conn_oauth_client_id_func clientid_impl,
				 conn_oauth_client_secret_func clientsecret_impl,
				 conn_oauth_discovery_uri_func discoveryuri_impl,
				 conn_oauth_issuer_id_func issuerid_impl,
				 conn_oauth_scope_func scope_impl,
				 conn_sasl_state_func saslstate_impl,
				 set_conn_altsock_func setaltsock_impl,
				 set_conn_oauth_token_func settoken_impl)
{
	pg_g_threadlock = threadlock_impl;
	libpq_gettext_impl = gettext_impl;
	conn_errorMessage = errmsg_impl;
	conn_oauth_client_id = clientid_impl;
	conn_oauth_client_secret = clientsecret_impl;
	conn_oauth_discovery_uri = discoveryuri_impl;
	conn_oauth_issuer_id = issuerid_impl;
	conn_oauth_scope = scope_impl;
	conn_sasl_state = saslstate_impl;
	set_conn_altsock = setaltsock_impl;
	set_conn_oauth_token = settoken_impl;
}

/*
 * Append a formatted string to the error message buffer of the given
 * connection, after translating it.  This is a copy of libpq's internal API.
 */
void
libpq_append_conn_error(PGconn *conn, const char *fmt,...)
{
	int			save_errno = errno;
	bool		done;
	va_list		args;
	PQExpBuffer errorMessage = conn_errorMessage(conn);

	Assert(fmt[strlen(fmt) - 1] != '\n');

	if (PQExpBufferBroken(errorMessage))
		return;					/* already failed */

	/* Loop in case we have to retry after enlarging the buffer. */
	do
	{
		errno = save_errno;
		va_start(args, fmt);
		done = appendPQExpBufferVA(errorMessage, libpq_gettext(fmt), args);
		va_end(args);
	} while (!done);

	appendPQExpBufferChar(errorMessage, '\n');
}

#ifdef ENABLE_NLS

/*
 * A shim that defers to the actual libpq_gettext().
 */
char *
libpq_gettext(const char *msgid)
{
	if (!libpq_gettext_impl)
	{
		/*
		 * Possible if the libpq build didn't enable NLS but the libpq-oauth
		 * build did. That's an odd mismatch, but we can handle it.
		 *
		 * Note that callers of libpq_gettext() have to treat the return value
		 * as if it were const, because builds without NLS simply pass through
		 * their argument.
		 */
		return unconstify(char *, msgid);
	}

	return libpq_gettext_impl(msgid);
}

#endif							/* ENABLE_NLS */

/*
 * Returns true if the PGOAUTHDEBUG=UNSAFE flag is set in the environment.
 */
bool
oauth_unsafe_debugging_enabled(void)
{
	const char *env = getenv("PGOAUTHDEBUG");

	return (env && strcmp(env, "UNSAFE") == 0);
}

/*
 * Duplicate SOCK_ERRNO* definitions from libpq-int.h, for use by
 * pq_block/reset_sigpipe().
 */
#ifdef WIN32
#define SOCK_ERRNO (WSAGetLastError())
#define SOCK_ERRNO_SET(e) WSASetLastError(e)
#else
#define SOCK_ERRNO errno
#define SOCK_ERRNO_SET(e) (errno = (e))
#endif

/*
 *	Block SIGPIPE for this thread. This is a copy of libpq's internal API.
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
 *	Discard any pending SIGPIPE and reset the signal mask. This is a copy of
 *	libpq's internal API.
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
