/*-------------------------------------------------------------------------
 *
 * scram.h
 *	  Interface to libpq/scram.c
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/scram.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SCRAM_H
#define PG_SCRAM_H

/* Name of SCRAM-SHA-256 per IANA */
#define SCRAM_SHA256_NAME "SCRAM-SHA-256"

/* Status codes for message exchange */
#define SASL_EXCHANGE_CONTINUE		0
#define SASL_EXCHANGE_SUCCESS		1
#define SASL_EXCHANGE_FAILURE		2

/* Routines dedicated to authentication */
extern void *pg_be_scram_init(const char *username, const char *shadow_pass);
extern int pg_be_scram_exchange(void *opaq, char *input, int inputlen,
					 char **output, int *outputlen, char **logdetail);

/* Routines to handle and check SCRAM-SHA-256 verifier */
extern char *pg_be_scram_build_verifier(const char *password);
extern bool scram_verify_plain_password(const char *username,
							const char *password, const char *verifier);

#endif							/* PG_SCRAM_H */
