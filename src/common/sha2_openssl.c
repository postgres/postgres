/*-------------------------------------------------------------------------
 *
 * sha2_openssl.c
 *	  Set of wrapper routines on top of OpenSSL to support SHA-224
 *	  SHA-256, SHA-384 and SHA-512 functions.
 *
 * This should only be used if code is compiled with OpenSSL support.
 *
 * Portions Copyright (c) 2016-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/common/sha2_openssl.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <openssl/sha.h>

#include "common/sha2.h"


/* Interface routines for SHA-256 */
void
pg_sha256_init(pg_sha256_ctx *ctx)
{
	SHA256_Init((SHA256_CTX *) ctx);
}

void
pg_sha256_update(pg_sha256_ctx *ctx, const uint8 *data, size_t len)
{
	SHA256_Update((SHA256_CTX *) ctx, data, len);
}

void
pg_sha256_final(pg_sha256_ctx *ctx, uint8 *dest)
{
	SHA256_Final(dest, (SHA256_CTX *) ctx);
}

/* Interface routines for SHA-512 */
void
pg_sha512_init(pg_sha512_ctx *ctx)
{
	SHA512_Init((SHA512_CTX *) ctx);
}

void
pg_sha512_update(pg_sha512_ctx *ctx, const uint8 *data, size_t len)
{
	SHA512_Update((SHA512_CTX *) ctx, data, len);
}

void
pg_sha512_final(pg_sha512_ctx *ctx, uint8 *dest)
{
	SHA512_Final(dest, (SHA512_CTX *) ctx);
}

/* Interface routines for SHA-384 */
void
pg_sha384_init(pg_sha384_ctx *ctx)
{
	SHA384_Init((SHA512_CTX *) ctx);
}

void
pg_sha384_update(pg_sha384_ctx *ctx, const uint8 *data, size_t len)
{
	SHA384_Update((SHA512_CTX *) ctx, data, len);
}

void
pg_sha384_final(pg_sha384_ctx *ctx, uint8 *dest)
{
	SHA384_Final(dest, (SHA512_CTX *) ctx);
}

/* Interface routines for SHA-224 */
void
pg_sha224_init(pg_sha224_ctx *ctx)
{
	SHA224_Init((SHA256_CTX *) ctx);
}

void
pg_sha224_update(pg_sha224_ctx *ctx, const uint8 *data, size_t len)
{
	SHA224_Update((SHA256_CTX *) ctx, data, len);
}

void
pg_sha224_final(pg_sha224_ctx *ctx, uint8 *dest)
{
	SHA224_Final(dest, (SHA256_CTX *) ctx);
}
