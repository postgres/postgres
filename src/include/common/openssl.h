/*-------------------------------------------------------------------------
 *
 * openssl.h
 *	  OpenSSL supporting functionality shared between frontend and backend
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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

/*
 * OpenSSL doesn't provide any very nice way to identify the min/max
 * protocol versions the library supports, so we fake it as best we can.
 * Note in particular that this doesn't account for restrictions that
 * might be specified in the installation's openssl.cnf.
 *
 * We disable SSLv3 and older in library setup, so TLSv1 is the oldest
 * protocol version of interest.
 */
#define MIN_OPENSSL_TLS_VERSION  "TLSv1"

#if defined(TLS1_3_VERSION)
#define MAX_OPENSSL_TLS_VERSION  "TLSv1.3"
#elif defined(TLS1_2_VERSION)
#define MAX_OPENSSL_TLS_VERSION  "TLSv1.2"
#elif defined(TLS1_1_VERSION)
#define MAX_OPENSSL_TLS_VERSION  "TLSv1.1"
#else
#define MAX_OPENSSL_TLS_VERSION  "TLSv1"
#endif

/* src/common/protocol_openssl.c */
#ifndef SSL_CTX_set_min_proto_version
extern int	SSL_CTX_set_min_proto_version(SSL_CTX *ctx, int version);
extern int	SSL_CTX_set_max_proto_version(SSL_CTX *ctx, int version);
#endif

#endif							/* USE_OPENSSL */

#endif							/* COMMON_OPENSSL_H */
