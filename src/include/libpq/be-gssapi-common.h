/*-------------------------------------------------------------------------
 *
 * be-gssapi-common.h
 *       Definitions for GSSAPI authentication and encryption handling
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/be-gssapi-common.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef BE_GSSAPI_COMMON_H
#define BE_GSSAPI_COMMON_H

#ifdef ENABLE_GSS

#if defined(HAVE_GSSAPI_H)
#include <gssapi.h>
#include <gssapi_ext.h>
#else
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_ext.h>
#endif

extern void pg_GSS_error(const char *errmsg,
						 OM_uint32 maj_stat, OM_uint32 min_stat);

extern void pg_store_delegated_credential(gss_cred_id_t cred);
#endif							/* ENABLE_GSS */

#endif							/* BE_GSSAPI_COMMON_H */
