/*-------------------------------------------------------------------------
 *
 * fe-auth-oauth.h
 *
 *	  Definitions for OAuth authentication implementations
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-oauth.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef FE_AUTH_OAUTH_H
#define FE_AUTH_OAUTH_H

#include "libpq-fe.h"
#include "libpq-int.h"


enum fe_oauth_step
{
	FE_OAUTH_INIT,
	FE_OAUTH_BEARER_SENT,
	FE_OAUTH_REQUESTING_TOKEN,
	FE_OAUTH_SERVER_ERROR,
};

typedef struct
{
	enum fe_oauth_step step;

	PGconn	   *conn;
	void	   *async_ctx;
} fe_oauth_state;

extern PostgresPollingStatusType pg_fe_run_oauth_flow(PGconn *conn);
extern void pg_fe_cleanup_oauth_flow(PGconn *conn);
extern void pqClearOAuthToken(PGconn *conn);
extern bool oauth_unsafe_debugging_enabled(void);

/* Mechanisms in fe-auth-oauth.c */
extern const pg_fe_sasl_mech pg_oauth_mech;

#endif							/* FE_AUTH_OAUTH_H */
