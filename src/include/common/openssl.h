/*-------------------------------------------------------------------------
 *
 * openssl.h
 *	  OpenSSL supporting functionality shared between frontend and backend
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/include/common/openssl.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMMON_OPENSSL_H
#define COMMON_OPENSSL_H

#ifdef USE_OPENSSL
#include <openssl/ssl.h>

/* src/common/protocol_openssl.c */
#ifndef SSL_CTX_set_min_proto_version
extern int	SSL_CTX_set_min_proto_version(SSL_CTX *ctx, int version);
extern int	SSL_CTX_set_max_proto_version(SSL_CTX *ctx, int version);
#endif

#endif

#endif							/* COMMON_OPENSSL_H */
