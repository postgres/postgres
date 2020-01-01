/*-------------------------------------------------------------------------
 *
 * fe-auth.h
 *
 *	  Definitions for network authentication routines
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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
extern char *pg_fe_getauthname(PQExpBuffer errorMessage);

/* Prototypes for functions in fe-auth-scram.c */
extern void *pg_fe_scram_init(PGconn *conn,
							  const char *password,
							  const char *sasl_mechanism);
extern bool pg_fe_scram_channel_bound(void *opaq);
extern void pg_fe_scram_free(void *opaq);
extern void pg_fe_scram_exchange(void *opaq, char *input, int inputlen,
								 char **output, int *outputlen,
								 bool *done, bool *success);
extern char *pg_fe_scram_build_secret(const char *password);

#endif							/* FE_AUTH_H */
