/*-------------------------------------------------------------------------
 *
 * cryptohash_openssl.c
 *	  Set of wrapper routines on top of OpenSSL to support cryptographic
 *	  hash functions.
 *
 * This should only be used if code is compiled with OpenSSL support.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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

#include <openssl/sha.h>

#include "common/cryptohash.h"

/*
 * In backend, use palloc/pfree to ease the error handling.  In frontend,
 * use malloc to be able to return a failure status back to the caller.
 */
#ifndef FRONTEND
#define ALLOC(size) palloc(size)
#define FREE(ptr) pfree(ptr)
#else
#define ALLOC(size) malloc(size)
#define FREE(ptr) free(ptr)
#endif

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

	ctx = ALLOC(sizeof(pg_cryptohash_ctx));
	if (ctx == NULL)
		return NULL;

	ctx->type = type;

	switch (type)
	{
		case PG_SHA224:
		case PG_SHA256:
			ctx->data = ALLOC(sizeof(SHA256_CTX));
			break;
		case PG_SHA384:
		case PG_SHA512:
			ctx->data = ALLOC(sizeof(SHA512_CTX));
			break;
	}

	if (ctx->data == NULL)
	{
		explicit_bzero(ctx, sizeof(pg_cryptohash_ctx));
		FREE(ctx);
		return NULL;
	}

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
		return 0;

	switch (ctx->type)
	{
		case PG_SHA224:
			status = SHA224_Init((SHA256_CTX *) ctx->data);
			break;
		case PG_SHA256:
			status = SHA256_Init((SHA256_CTX *) ctx->data);
			break;
		case PG_SHA384:
			status = SHA384_Init((SHA512_CTX *) ctx->data);
			break;
		case PG_SHA512:
			status = SHA512_Init((SHA512_CTX *) ctx->data);
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
		return 0;

	switch (ctx->type)
	{
		case PG_SHA224:
			status = SHA224_Update((SHA256_CTX *) ctx->data, data, len);
			break;
		case PG_SHA256:
			status = SHA256_Update((SHA256_CTX *) ctx->data, data, len);
			break;
		case PG_SHA384:
			status = SHA384_Update((SHA512_CTX *) ctx->data, data, len);
			break;
		case PG_SHA512:
			status = SHA512_Update((SHA512_CTX *) ctx->data, data, len);
			break;
	}

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
pg_cryptohash_final(pg_cryptohash_ctx *ctx, uint8 *dest)
{
	int			status = 0;

	if (ctx == NULL)
		return 0;

	switch (ctx->type)
	{
		case PG_SHA224:
			status = SHA224_Final(dest, (SHA256_CTX *) ctx->data);
			break;
		case PG_SHA256:
			status = SHA256_Final(dest, (SHA256_CTX *) ctx->data);
			break;
		case PG_SHA384:
			status = SHA384_Final(dest, (SHA512_CTX *) ctx->data);
			break;
		case PG_SHA512:
			status = SHA512_Final(dest, (SHA512_CTX *) ctx->data);
			break;
	}

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
	FREE(ctx->data);
	explicit_bzero(ctx, sizeof(pg_cryptohash_ctx));
	FREE(ctx);
}
