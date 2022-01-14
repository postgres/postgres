/*-------------------------------------------------------------------------
 *
 * cryptohash_openssl.c
 *	  Set of wrapper routines on top of OpenSSL to support cryptographic
 *	  hash functions.
 *
 * This should only be used if code is compiled with OpenSSL support.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/common/cryptohash_openssl.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <openssl/evp.h>

#include "common/cryptohash.h"
#include "common/md5.h"
#include "common/sha1.h"
#include "common/sha2.h"
#ifndef FRONTEND
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/resowner_private.h"
#endif

/*
 * In the backend, use an allocation in TopMemoryContext to count for
 * resowner cleanup handling.  In the frontend, use malloc to be able
 * to return a failure status back to the caller.
 */
#ifndef FRONTEND
#define ALLOC(size) MemoryContextAlloc(TopMemoryContext, size)
#define FREE(ptr) pfree(ptr)
#else
#define ALLOC(size) malloc(size)
#define FREE(ptr) free(ptr)
#endif

/*
 * Internal pg_cryptohash_ctx structure.
 *
 * This tracks the resource owner associated to each EVP context data
 * for the backend.
 */
struct pg_cryptohash_ctx
{
	pg_cryptohash_type type;

	EVP_MD_CTX *evpctx;

#ifndef FRONTEND
	ResourceOwner resowner;
#endif
};

/*
 * pg_cryptohash_create
 *
 * Allocate a hash context.  Returns NULL on failure for an OOM.  The
 * backend issues an error, without returning.
 */
pg_cryptohash_ctx *
pg_cryptohash_create(pg_cryptohash_type type)
{
	pg_cryptohash_ctx *ctx;

	/*
	 * Make sure that the resource owner has space to remember this reference.
	 * This can error out with "out of memory", so do this before any other
	 * allocation to avoid leaking.
	 */
#ifndef FRONTEND
	ResourceOwnerEnlargeCryptoHash(CurrentResourceOwner);
#endif

	ctx = ALLOC(sizeof(pg_cryptohash_ctx));
	if (ctx == NULL)
		return NULL;
	memset(ctx, 0, sizeof(pg_cryptohash_ctx));
	ctx->type = type;

	/*
	 * Initialization takes care of assigning the correct type for OpenSSL.
	 */
	ctx->evpctx = EVP_MD_CTX_create();

	if (ctx->evpctx == NULL)
	{
		explicit_bzero(ctx, sizeof(pg_cryptohash_ctx));
		FREE(ctx);
#ifndef FRONTEND
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
#else
		return NULL;
#endif
	}

#ifndef FRONTEND
	ctx->resowner = CurrentResourceOwner;
	ResourceOwnerRememberCryptoHash(CurrentResourceOwner,
									PointerGetDatum(ctx));
#endif

	return ctx;
}

/*
 * pg_cryptohash_init
 *
 * Initialize a hash context.  Returns 0 on success, and -1 on failure.
 */
int
pg_cryptohash_init(pg_cryptohash_ctx *ctx)
{
	int			status = 0;

	if (ctx == NULL)
		return -1;

	switch (ctx->type)
	{
		case PG_MD5:
			status = EVP_DigestInit_ex(ctx->evpctx, EVP_md5(), NULL);
			break;
		case PG_SHA1:
			status = EVP_DigestInit_ex(ctx->evpctx, EVP_sha1(), NULL);
			break;
		case PG_SHA224:
			status = EVP_DigestInit_ex(ctx->evpctx, EVP_sha224(), NULL);
			break;
		case PG_SHA256:
			status = EVP_DigestInit_ex(ctx->evpctx, EVP_sha256(), NULL);
			break;
		case PG_SHA384:
			status = EVP_DigestInit_ex(ctx->evpctx, EVP_sha384(), NULL);
			break;
		case PG_SHA512:
			status = EVP_DigestInit_ex(ctx->evpctx, EVP_sha512(), NULL);
			break;
	}

	/* OpenSSL internals return 1 on success, 0 on failure */
	if (status <= 0)
		return -1;
	return 0;
}

/*
 * pg_cryptohash_update
 *
 * Update a hash context.  Returns 0 on success, and -1 on failure.
 */
int
pg_cryptohash_update(pg_cryptohash_ctx *ctx, const uint8 *data, size_t len)
{
	int			status = 0;

	if (ctx == NULL)
		return -1;

	status = EVP_DigestUpdate(ctx->evpctx, data, len);

	/* OpenSSL internals return 1 on success, 0 on failure */
	if (status <= 0)
		return -1;
	return 0;
}

/*
 * pg_cryptohash_final
 *
 * Finalize a hash context.  Returns 0 on success, and -1 on failure.
 */
int
pg_cryptohash_final(pg_cryptohash_ctx *ctx, uint8 *dest, size_t len)
{
	int			status = 0;

	if (ctx == NULL)
		return -1;

	switch (ctx->type)
	{
		case PG_MD5:
			if (len < MD5_DIGEST_LENGTH)
				return -1;
			break;
		case PG_SHA1:
			if (len < SHA1_DIGEST_LENGTH)
				return -1;
			break;
		case PG_SHA224:
			if (len < PG_SHA224_DIGEST_LENGTH)
				return -1;
			break;
		case PG_SHA256:
			if (len < PG_SHA256_DIGEST_LENGTH)
				return -1;
			break;
		case PG_SHA384:
			if (len < PG_SHA384_DIGEST_LENGTH)
				return -1;
			break;
		case PG_SHA512:
			if (len < PG_SHA512_DIGEST_LENGTH)
				return -1;
			break;
	}

	status = EVP_DigestFinal_ex(ctx->evpctx, dest, 0);

	/* OpenSSL internals return 1 on success, 0 on failure */
	if (status <= 0)
		return -1;
	return 0;
}

/*
 * pg_cryptohash_free
 *
 * Free a hash context.
 */
void
pg_cryptohash_free(pg_cryptohash_ctx *ctx)
{
	if (ctx == NULL)
		return;

	EVP_MD_CTX_destroy(ctx->evpctx);

#ifndef FRONTEND
	ResourceOwnerForgetCryptoHash(ctx->resowner,
								  PointerGetDatum(ctx));
#endif

	explicit_bzero(ctx, sizeof(pg_cryptohash_ctx));
	FREE(ctx);
}
