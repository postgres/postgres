/*-------------------------------------------------------------------------
 *
 * fe-auth.h
 *
 *	  Definitions for network authentication routines
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FE_AUTH_H
#define FE_AUTH_H

#include "libpq-fe.h"
#include "libpq-int.h"


/* Prototypes for functions in fe-auth.c */
extern int	pg_fe_sendauth(AuthRequest areq, int payloadlen, PGconn *conn);
extern char *pg_fe_getusername(uid_t user_id, PQExpBuffer errorMessage);
extern char *pg_fe_getauthname(PQExpBuffer errorMessage);

/* Mechanisms in fe-auth-scram.c */
extern const pg_fe_sasl_mech pg_scram_mech;
extern char *pg_fe_scram_build_secret(const char *password,
									  const char **errstr);

#endif							/* FE_AUTH_H */
