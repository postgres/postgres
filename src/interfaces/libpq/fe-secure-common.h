/*-------------------------------------------------------------------------
 *
 * fe-secure-common.h
 *
 * common implementation-independent SSL support code
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-secure-common.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef FE_SECURE_COMMON_H
#define FE_SECURE_COMMON_H

#include "libpq-fe.h"

extern int	pq_verify_peer_name_matches_certificate_name(PGconn *conn,
														 const char *namedata, size_t namelen,
														 char **store_name);
extern int	pq_verify_peer_name_matches_certificate_ip(PGconn *conn,
													   const unsigned char *ipdata,
													   size_t iplen,
													   char **store_name);
extern bool pq_verify_peer_name_matches_certificate(PGconn *conn);

#endif							/* FE_SECURE_COMMON_H */
