/*-------------------------------------------------------------------------
 *
 * oauth.h
 *	  Interface to libpq/auth-oauth.c
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/oauth.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_OAUTH_H
#define PG_OAUTH_H

#include "libpq/libpq-be.h"
#include "libpq/sasl.h"

extern char *oauth_validator_command;

/* Implementation */
extern const pg_be_sasl_mech pg_be_oauth_mech;

#endif /* PG_OAUTH_H */
