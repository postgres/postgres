/*-------------------------------------------------------------------------
 *
 * scram.h
 *	  Interface to libpq/scram.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/scram.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SCRAM_H
#define PG_SCRAM_H

#include "common/cryptohash.h"
#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"
#include "libpq/sasl.h"

/* Number of iterations when generating new secrets */
extern PGDLLIMPORT int scram_sha_256_iterations;

/* SASL implementation callbacks */
extern PGDLLIMPORT const pg_be_sasl_mech pg_be_scram_mech;

/* Routines to handle and check SCRAM-SHA-256 secret */
extern char *pg_be_scram_build_secret(const char *password);
extern bool parse_scram_secret(const char *secret,
							   int *iterations,
							   pg_cryptohash_type *hash_type,
							   int *key_length, char **salt,
							   uint8 *stored_key, uint8 *server_key);
extern bool scram_verify_plain_password(const char *username,
										const char *password, const char *secret);

#endif							/* PG_SCRAM_H */
