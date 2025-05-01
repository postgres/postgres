/*-------------------------------------------------------------------------
 *
 * oauth-utils.h
 *
 *	  Definitions providing missing libpq internal APIs
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq-oauth/oauth-utils.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef OAUTH_UTILS_H
#define OAUTH_UTILS_H

#include "fe-auth-oauth.h"
#include "libpq-fe.h"
#include "pqexpbuffer.h"

/*
 * A bank of callbacks to safely access members of PGconn, which are all passed
 * to libpq_oauth_init() by libpq.
 *
 * Keep these aligned with the definitions in fe-auth-oauth.c as well as the
 * static declarations in oauth-curl.c.
 */
#define DECLARE_GETTER(TYPE, MEMBER) \
	typedef TYPE (*conn_ ## MEMBER ## _func) (PGconn *conn); \
	extern conn_ ## MEMBER ## _func conn_ ## MEMBER;

#define DECLARE_SETTER(TYPE, MEMBER) \
	typedef void (*set_conn_ ## MEMBER ## _func) (PGconn *conn, TYPE val); \
	extern set_conn_ ## MEMBER ## _func set_conn_ ## MEMBER;

DECLARE_GETTER(PQExpBuffer, errorMessage);
DECLARE_GETTER(char *, oauth_client_id);
DECLARE_GETTER(char *, oauth_client_secret);
DECLARE_GETTER(char *, oauth_discovery_uri);
DECLARE_GETTER(char *, oauth_issuer_id);
DECLARE_GETTER(char *, oauth_scope);
DECLARE_GETTER(fe_oauth_state *, sasl_state);

DECLARE_SETTER(pgsocket, altsock);
DECLARE_SETTER(char *, oauth_token);

#undef DECLARE_GETTER
#undef DECLARE_SETTER

typedef char *(*libpq_gettext_func) (const char *msgid);

/* Initializes libpq-oauth. */
extern PGDLLEXPORT void libpq_oauth_init(pgthreadlock_t threadlock,
										 libpq_gettext_func gettext_impl,
										 conn_errorMessage_func errmsg_impl,
										 conn_oauth_client_id_func clientid_impl,
										 conn_oauth_client_secret_func clientsecret_impl,
										 conn_oauth_discovery_uri_func discoveryuri_impl,
										 conn_oauth_issuer_id_func issuerid_impl,
										 conn_oauth_scope_func scope_impl,
										 conn_sasl_state_func saslstate_impl,
										 set_conn_altsock_func setaltsock_impl,
										 set_conn_oauth_token_func settoken_impl);

/*
 * Duplicated APIs, copied from libpq (primarily libpq-int.h, which we cannot
 * depend on here).
 */

typedef enum
{
	PG_BOOL_UNKNOWN = 0,		/* Currently unknown */
	PG_BOOL_YES,				/* Yes (true) */
	PG_BOOL_NO					/* No (false) */
} PGTernaryBool;

extern void libpq_append_conn_error(PGconn *conn, const char *fmt,...) pg_attribute_printf(2, 3);
extern bool oauth_unsafe_debugging_enabled(void);
extern int	pq_block_sigpipe(sigset_t *osigset, bool *sigpipe_pending);
extern void pq_reset_sigpipe(sigset_t *osigset, bool sigpipe_pending, bool got_epipe);

#ifdef ENABLE_NLS
extern char *libpq_gettext(const char *msgid) pg_attribute_format_arg(1);
#else
#define libpq_gettext(x) (x)
#endif

extern pgthreadlock_t pg_g_threadlock;

#define pglock_thread()		pg_g_threadlock(true)
#define pgunlock_thread()	pg_g_threadlock(false)

#endif							/* OAUTH_UTILS_H */
