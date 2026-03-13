/*-------------------------------------------------------------------------
 *
 * oauth-curl.h
 *
 *	  Definitions for OAuth Device Authorization module
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq-oauth/oauth-curl.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef OAUTH_CURL_H
#define OAUTH_CURL_H

#include "libpq-fe.h"

/* Exported flow callback. */
extern PGDLLEXPORT int pg_start_oauthbearer(PGconn *conn,
											PGoauthBearerRequestV2 *request);

#endif							/* OAUTH_CURL_H */
