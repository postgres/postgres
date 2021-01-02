/*-------------------------------------------------------------------------
 *
 * scram.h
 *	  Interface to libpq/scram.c
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/scram.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SCRAM_H
#define PG_SCRAM_H

#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"

/* Status codes for message exchange */
#define SASL_EXCHANGE_CONTINUE		0
#define SASL_EXCHANGE_SUCCESS		1
#define SASL_EXCHANGE_FAILURE		2

/* Routines dedicated to authentication */
extern void pg_be_scram_get_mechanisms(Port *port, StringInfo buf);
extern void *pg_be_scram_init(Port *port, const char *selected_mech, const char *shadow_pass);
extern int	pg_be_scram_exchange(void *opaq, const char *input, int inputlen,
								 char **output, int *outputlen, char **logdetail);

/* Routines to handle and check SCRAM-SHA-256 secret */
extern char *pg_be_scram_build_secret(const char *password);
extern bool parse_scram_secret(const char *secret, int *iterations, char **salt,
							   uint8 *stored_key, uint8 *server_key);
extern bool scram_verify_plain_password(const char *username,
										const char *password, const char *secret);

#endif							/* PG_SCRAM_H */
