/*-------------------------------------------------------------------------
 *
 * oauth-curl.h
 *
 *	  Definitions for OAuth Device Authorization module
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq-oauth/oauth-curl.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef OAUTH_CURL_H
#define OAUTH_CURL_H

#include "libpq-fe.h"

/* Exported async-auth callbacks. */
extern PGDLLEXPORT PostgresPollingStatusType pg_fe_run_oauth_flow(PGconn *conn);
extern PGDLLEXPORT void pg_fe_cleanup_oauth_flow(PGconn *conn);

#endif							/* OAUTH_CURL_H */
