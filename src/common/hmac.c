/*-------------------------------------------------------------------------
 *
 * hmac.c
 *	  Implements Keyed-Hashing for Message Authentication (HMAC)
 *
 * Fallback implementation of HMAC, as specified in RFC 2104.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/hmac.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/cryptohash.h"
#include "common/hmac.h"
#include "common/md5.h"
#include "common/sha1.h"
#include "common/sha2.h"

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

/* Set of error states */
typedef enum pg_hmac_errno
{
	PG_HMAC_ERROR_NONE = 0,
	PG_HMAC_ERROR_OOM,
	PG_HMAC_ERROR_INTERNAL,
} pg_hmac_errno;

/* Internal pg_hmac_ctx structure */
struct pg_hmac_ctx
{
	pg_cryptohash_ctx *hash;
	pg_cryptohash_type type;
	pg_hmac_errno error;
	const char *errreason;
	int			block_size;
	int			digest_size;

	/*
	 * Use the largest block size among supported options.  This wastes some
	 * memory but simplifies the allocation logic.
	 */
	uint8		k_ipad[PG_SHA512_BLOCK_LENGTH];
	uint8		k_opad[PG_SHA512_BLOCK_LENGTH];
};

#define HMAC_IPAD 0x36
#define HMAC_OPAD 0x5C

/*
 * pg_hmac_create
 *
 * Allocate a hash context.  Returns NULL on failure for an OOM.  The
 * backend issues an error, without returning.
 */
pg_hmac_ctx *
pg_hmac_create(pg_cryptohash_type type)
{
	pg_hmac_ctx *ctx;

	ctx = ALLOC(sizeof(pg_hmac_ctx));
	if (ctx == NULL)
		return NULL;
	memset(ctx, 0, sizeof(pg_hmac_ctx));
	ctx->type = type;
	ctx->error = PG_HMAC_ERROR_NONE;
	ctx->errreason = NULL;

	/*
	 * Initialize the context data.  This requires to know the digest and
	 * block lengths, that depend on the type of hash used.
	 */
	switch (type)
	{
		case PG_MD5:
			ctx->digest_size = MD5_DIGEST_LENGTH;
			ctx->block_size = MD5_BLOCK_SIZE;
			break;
		case PG_SHA1:
			ctx->digest_size = SHA1_DIGEST_LENGTH;
			ctx->block_size = SHA1_BLOCK_SIZE;
			break;
		case PG_SHA224:
			ctx->digest_size = PG_SHA224_DIGEST_LENGTH;
			ctx->block_size = PG_SHA224_BLOCK_LENGTH;
			break;
		case PG_SHA256:
			ctx->digest_size = PG_SHA256_DIGEST_LENGTH;
			ctx->block_size = PG_SHA256_BLOCK_LENGTH;
			break;
		case PG_SHA384:
			ctx->digest_size = PG_SHA384_DIGEST_LENGTH;
			ctx->block_size = PG_SHA384_BLOCK_LENGTH;
			break;
		case PG_SHA512:
			ctx->digest_size = PG_SHA512_DIGEST_LENGTH;
			ctx->block_size = PG_SHA512_BLOCK_LENGTH;
			break;
	}

	ctx->hash = pg_cryptohash_create(type);
	if (ctx->hash == NULL)
	{
		explicit_bzero(ctx, sizeof(pg_hmac_ctx));
		FREE(ctx);
		return NULL;
	}

	return ctx;
}

/*
 * pg_hmac_init
 *
 * Initialize a HMAC context.  Returns 0 on success, -1 on failure.
 */
int
pg_hmac_init(pg_hmac_ctx *ctx, const uint8 *key, size_t len)
{
	int			i;
	int			digest_size;
	int			block_size;
	uint8	   *shrinkbuf = NULL;

	if (ctx == NULL)
		return -1;

	digest_size = ctx->digest_size;
	block_size = ctx->block_size;

	memset(ctx->k_opad, HMAC_OPAD, ctx->block_size);
	memset(ctx->k_ipad, HMAC_IPAD, ctx->block_size);

	/*
	 * If the key is longer than the block size, pass it through the hash once
	 * to shrink it down.
	 */
	if (len > block_size)
	{
		pg_cryptohash_ctx *hash_ctx;

		/* temporary buffer for one-time shrink */
		shrinkbuf = ALLOC(digest_size);
		if (shrinkbuf == NULL)
		{
			ctx->error = PG_HMAC_ERROR_OOM;
			return -1;
		}
		memset(shrinkbuf, 0, digest_size);

		hash_ctx = pg_cryptohash_create(ctx->type);
		if (hash_ctx == NULL)
		{
			ctx->error = PG_HMAC_ERROR_OOM;
			FREE(shrinkbuf);
			return -1;
		}

		if (pg_cryptohash_init(hash_ctx) < 0 ||
			pg_cryptohash_update(hash_ctx, key, len) < 0 ||
			pg_cryptohash_final(hash_ctx, shrinkbuf, digest_size) < 0)
		{
			ctx->error = PG_HMAC_ERROR_INTERNAL;
			ctx->errreason = pg_cryptohash_error(hash_ctx);
			pg_cryptohash_free(hash_ctx);
			FREE(shrinkbuf);
			return -1;
		}

		key = shrinkbuf;
		len = digest_size;
		pg_cryptohash_free(hash_ctx);
	}

	for (i = 0; i < len; i++)
	{
		ctx->k_ipad[i] ^= key[i];
		ctx->k_opad[i] ^= key[i];
	}

	/* tmp = H(K XOR ipad, text) */
	if (pg_cryptohash_init(ctx->hash) < 0 ||
		pg_cryptohash_update(ctx->hash, ctx->k_ipad, ctx->block_size) < 0)
	{
		ctx->error = PG_HMAC_ERROR_INTERNAL;
		ctx->errreason = pg_cryptohash_error(ctx->hash);
		if (shrinkbuf)
			FREE(shrinkbuf);
		return -1;
	}

	if (shrinkbuf)
		FREE(shrinkbuf);
	return 0;
}

/*
 * pg_hmac_update
 *
 * Update a HMAC context.  Returns 0 on success, -1 on failure.
 */
int
pg_hmac_update(pg_hmac_ctx *ctx, const uint8 *data, size_t len)
{
	if (ctx == NULL)
		return -1;

	if (pg_cryptohash_update(ctx->hash, data, len) < 0)
	{
		ctx->error = PG_HMAC_ERROR_INTERNAL;
		ctx->errreason = pg_cryptohash_error(ctx->hash);
		return -1;
	}

	return 0;
}

/*
 * pg_hmac_final
 *
 * Finalize a HMAC context.  Returns 0 on success, -1 on failure.
 */
int
pg_hmac_final(pg_hmac_ctx *ctx, uint8 *dest, size_t len)
{
	uint8	   *h;

	if (ctx == NULL)
		return -1;

	h = ALLOC(ctx->digest_size);
	if (h == NULL)
	{
		ctx->error = PG_HMAC_ERROR_OOM;
		return -1;
	}
	memset(h, 0, ctx->digest_size);

	if (pg_cryptohash_final(ctx->hash, h, ctx->digest_size) < 0)
	{
		ctx->error = PG_HMAC_ERROR_INTERNAL;
		ctx->errreason = pg_cryptohash_error(ctx->hash);
		FREE(h);
		return -1;
	}

	/* H(K XOR opad, tmp) */
	if (pg_cryptohash_init(ctx->hash) < 0 ||
		pg_cryptohash_update(ctx->hash, ctx->k_opad, ctx->block_size) < 0 ||
		pg_cryptohash_update(ctx->hash, h, ctx->digest_size) < 0 ||
		pg_cryptohash_final(ctx->hash, dest, len) < 0)
	{
		ctx->error = PG_HMAC_ERROR_INTERNAL;
		ctx->errreason = pg_cryptohash_error(ctx->hash);
		FREE(h);
		return -1;
	}

	FREE(h);
	return 0;
}

/*
 * pg_hmac_free
 *
 * Free a HMAC context.
 */
void
pg_hmac_free(pg_hmac_ctx *ctx)
{
	if (ctx == NULL)
		return;

	pg_cryptohash_free(ctx->hash);
	explicit_bzero(ctx, sizeof(pg_hmac_ctx));
	FREE(ctx);
}

/*
 * pg_hmac_error
 *
 * Returns a static string providing details about an error that happened
 * during a HMAC computation.
 */
const char *
pg_hmac_error(pg_hmac_ctx *ctx)
{
	if (ctx == NULL)
		return _("out of memory");

	/*
	 * If a reason is provided, rely on it, else fallback to any error code
	 * set.
	 */
	if (ctx->errreason)
		return ctx->errreason;

	switch (ctx->error)
	{
		case PG_HMAC_ERROR_NONE:
			return _("success");
		case PG_HMAC_ERROR_INTERNAL:
			return _("internal error");
		case PG_HMAC_ERROR_OOM:
			return _("out of memory");
	}

	Assert(false);				/* cannot be reached */
	return _("success");
}
