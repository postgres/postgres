/*-------------------------------------------------------------------------
 *
 * protocol_openssl.c
 *	  OpenSSL functionality shared between frontend and backend
 *
 * This should only be used if code is compiled with OpenSSL support.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/common/protocol_openssl.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/openssl.h"

/*
 * Replacements for APIs introduced in OpenSSL 1.1.0.
 */
#ifndef SSL_CTX_set_min_proto_version

/*
 * OpenSSL versions that support TLS 1.3 shouldn't get here because they
 * already have these functions.  So we don't have to keep updating the below
 * code for every new TLS version, and eventually it can go away.  But let's
 * just check this to make sure ...
 */
#ifdef TLS1_3_VERSION
#error OpenSSL version mismatch
#endif

int
SSL_CTX_set_min_proto_version(SSL_CTX *ctx, int version)
{
	int			ssl_options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;

	if (version > TLS1_VERSION)
		ssl_options |= SSL_OP_NO_TLSv1;

	/*
	 * Some OpenSSL versions define TLS*_VERSION macros but not the
	 * corresponding SSL_OP_NO_* macro, so in those cases we have to return
	 * unsuccessfully here.
	 */
#ifdef TLS1_1_VERSION
	if (version > TLS1_1_VERSION)
	{
#ifdef SSL_OP_NO_TLSv1_1
		ssl_options |= SSL_OP_NO_TLSv1_1;
#else
		return 0;
#endif
	}
#endif
#ifdef TLS1_2_VERSION
	if (version > TLS1_2_VERSION)
	{
#ifdef SSL_OP_NO_TLSv1_2
		ssl_options |= SSL_OP_NO_TLSv1_2;
#else
		return 0;
#endif
	}
#endif

	SSL_CTX_set_options(ctx, ssl_options);

	return 1;					/* success */
}

int
SSL_CTX_set_max_proto_version(SSL_CTX *ctx, int version)
{
	int			ssl_options = 0;

	AssertArg(version != 0);

	/*
	 * Some OpenSSL versions define TLS*_VERSION macros but not the
	 * corresponding SSL_OP_NO_* macro, so in those cases we have to return
	 * unsuccessfully here.
	 */
#ifdef TLS1_1_VERSION
	if (version < TLS1_1_VERSION)
	{
#ifdef SSL_OP_NO_TLSv1_1
		ssl_options |= SSL_OP_NO_TLSv1_1;
#else
		return 0;
#endif
	}
#endif
#ifdef TLS1_2_VERSION
	if (version < TLS1_2_VERSION)
	{
#ifdef SSL_OP_NO_TLSv1_2
		ssl_options |= SSL_OP_NO_TLSv1_2;
#else
		return 0;
#endif
	}
#endif

	SSL_CTX_set_options(ctx, ssl_options);

	return 1;					/* success */
}

#endif							/* !SSL_CTX_set_min_proto_version */
