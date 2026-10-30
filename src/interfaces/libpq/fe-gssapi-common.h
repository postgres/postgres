/*-------------------------------------------------------------------------
 *
 * fe-gssapi-common.h
 *
 *      Definitions for GSSAPI common routines
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-gssapi-common.h
 */

#ifndef FE_GSSAPI_COMMON_H
#define FE_GSSAPI_COMMON_H

#include "libpq-fe.h"
#include "libpq-int.h"

#ifdef ENABLE_GSS

void		pg_GSS_error(const char *mprefix, PGconn *conn,
						 OM_uint32 maj_stat, OM_uint32 min_stat);
gss_cred_id_t pg_GSS_get_cred_cache(void);
int			pg_GSS_load_servicename(PGconn *conn);

#endif

#endif							/* FE_GSSAPI_COMMON_H */
