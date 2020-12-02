/*-------------------------------------------------------------------------
 *
 * cryptohash.c
 *	  Fallback implementations for cryptographic hash functions.
 *
 * This is the set of in-core functions used when there are no other
 * alternative options like OpenSSL.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/cryptohash.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <sys/param.h>

#include "common/cryptohash.h"
#include "sha2_int.h"

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
			ctx->data = ALLOC(sizeof(pg_sha224_ctx));
			break;
		case PG_SHA256:
			ctx->data = ALLOC(sizeof(pg_sha256_ctx));
			break;
		case PG_SHA384:
			ctx->data = ALLOC(sizeof(pg_sha384_ctx));
			break;
		case PG_SHA512:
			ctx->data = ALLOC(sizeof(pg_sha512_ctx));
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
 * Initialize a hash context.  Note that this implementation is designed
 * to never fail, so this always returns 0.
 */
int
pg_cryptohash_init(pg_cryptohash_ctx *ctx)
{
	if (ctx == NULL)
		return 0;

	switch (ctx->type)
	{
		case PG_SHA224:
			pg_sha224_init((pg_sha224_ctx *) ctx->data);
			break;
		case PG_SHA256:
			pg_sha256_init((pg_sha256_ctx *) ctx->data);
			break;
		case PG_SHA384:
			pg_sha384_init((pg_sha384_ctx *) ctx->data);
			break;
		case PG_SHA512:
			pg_sha512_init((pg_sha512_ctx *) ctx->data);
			break;
	}

	return 0;
}

/*
 * pg_cryptohash_update
 *
 * Update a hash context.  Note that this implementation is designed
 * to never fail, so this always returns 0.
 */
int
pg_cryptohash_update(pg_cryptohash_ctx *ctx, const uint8 *data, size_t len)
{
	if (ctx == NULL)
		return 0;

	switch (ctx->type)
	{
		case PG_SHA224:
			pg_sha224_update((pg_sha224_ctx *) ctx->data, data, len);
			break;
		case PG_SHA256:
			pg_sha256_update((pg_sha256_ctx *) ctx->data, data, len);
			break;
		case PG_SHA384:
			pg_sha384_update((pg_sha384_ctx *) ctx->data, data, len);
			break;
		case PG_SHA512:
			pg_sha512_update((pg_sha512_ctx *) ctx->data, data, len);
			break;
	}

	return 0;
}

/*
 * pg_cryptohash_final
 *
 * Finalize a hash context.  Note that this implementation is designed
 * to never fail, so this always returns 0.
 */
int
pg_cryptohash_final(pg_cryptohash_ctx *ctx, uint8 *dest)
{
	if (ctx == NULL)
		return 0;

	switch (ctx->type)
	{
		case PG_SHA224:
			pg_sha224_final((pg_sha224_ctx *) ctx->data, dest);
			break;
		case PG_SHA256:
			pg_sha256_final((pg_sha256_ctx *) ctx->data, dest);
			break;
		case PG_SHA384:
			pg_sha384_final((pg_sha384_ctx *) ctx->data, dest);
			break;
		case PG_SHA512:
			pg_sha512_final((pg_sha512_ctx *) ctx->data, dest);
			break;
	}

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
