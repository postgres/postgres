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

#include "common/sha2.h"

#ifdef FRONTEND
#include "common/logging.h"
#else
#include "miscadmin.h"
#endif

#ifdef FRONTEND
#define sha2_log_and_abort(...) \
	do { pg_log_fatal(__VA_ARGS__); exit(1); } while(0)
#else
#define sha2_log_and_abort(...) elog(ERROR, __VA_ARGS__)
#endif

static void
digest_init(EVP_MD_CTX **ctx, const EVP_MD *type)
{
	*ctx = EVP_MD_CTX_create();
	if (*ctx == NULL)
		sha2_log_and_abort("could not create EVP digest context");
	if (EVP_DigestInit_ex(*ctx, type, NULL) <= 0)
		sha2_log_and_abort("could not initialize EVP digest context");
}

static void
digest_update(EVP_MD_CTX **ctx, const uint8 *data, size_t len)
{
	if (EVP_DigestUpdate(*ctx, data, len) <= 0)
		sha2_log_and_abort("could not update EVP digest context");
}

static void
digest_final(EVP_MD_CTX **ctx, uint8 *dest)
{
	if (EVP_DigestFinal_ex(*ctx, dest, 0) <= 0)
		sha2_log_and_abort("could not finalize EVP digest context");
	EVP_MD_CTX_destroy(*ctx);
}

/* Interface routines for SHA-256 */
void
pg_sha256_init(pg_sha256_ctx *ctx)
{
	digest_init(ctx, EVP_sha256());
}

void
pg_sha256_update(pg_sha256_ctx *ctx, const uint8 *data, size_t len)
{
	digest_update(ctx, data, len);
}

void
pg_sha256_final(pg_sha256_ctx *ctx, uint8 *dest)
{
	digest_final(ctx, dest);
}

/* Interface routines for SHA-512 */
void
pg_sha512_init(pg_sha512_ctx *ctx)
{
	digest_init(ctx, EVP_sha512());
}

void
pg_sha512_update(pg_sha512_ctx *ctx, const uint8 *data, size_t len)
{
	digest_update(ctx, data, len);
}

void
pg_sha512_final(pg_sha512_ctx *ctx, uint8 *dest)
{
	digest_final(ctx, dest);
}

/* Interface routines for SHA-384 */
void
pg_sha384_init(pg_sha384_ctx *ctx)
{
	digest_init(ctx, EVP_sha384());
}

void
pg_sha384_update(pg_sha384_ctx *ctx, const uint8 *data, size_t len)
{
	digest_update(ctx, data, len);
}

void
pg_sha384_final(pg_sha384_ctx *ctx, uint8 *dest)
{
	digest_final(ctx, dest);
}

/* Interface routines for SHA-224 */
void
pg_sha224_init(pg_sha224_ctx *ctx)
{
	digest_init(ctx, EVP_sha224());
}

void
pg_sha224_update(pg_sha224_ctx *ctx, const uint8 *data, size_t len)
{
	digest_update(ctx, data, len);
}

void
pg_sha224_final(pg_sha224_ctx *ctx, uint8 *dest)
{
	digest_final(ctx, dest);
}
