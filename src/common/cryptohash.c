/*-------------------------------------------------------------------------
 *
 * cryptohash.c
 *	  Fallback implementations for cryptographic hash functions.
 *
 * This is the set of in-core functions used when there are no other
 * alternative options like OpenSSL.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "md5_int.h"
#include "sha1_int.h"
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

/* Set of error states */
typedef enum pg_cryptohash_errno
{
	PG_CRYPTOHASH_ERROR_NONE = 0,
	PG_CRYPTOHASH_ERROR_DEST_LEN,
} pg_cryptohash_errno;

/* Internal pg_cryptohash_ctx structure */
struct pg_cryptohash_ctx
{
	pg_cryptohash_type type;
	pg_cryptohash_errno error;

	union
	{
		pg_md5_ctx	md5;
		pg_sha1_ctx sha1;
		pg_sha224_ctx sha224;
		pg_sha256_ctx sha256;
		pg_sha384_ctx sha384;
		pg_sha512_ctx sha512;
	}			data;
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
	 * Note that this always allocates enough space for the largest hash. A
	 * smaller allocation would be enough for md5, sha224 and sha256, but the
	 * small extra amount of memory does not make it worth complicating this
	 * code.
	 */
	ctx = ALLOC(sizeof(pg_cryptohash_ctx));
	if (ctx == NULL)
		return NULL;

	memset(ctx, 0, sizeof(pg_cryptohash_ctx));
	ctx->type = type;
	ctx->error = PG_CRYPTOHASH_ERROR_NONE;
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
	if (ctx == NULL)
		return -1;

	switch (ctx->type)
	{
		case PG_MD5:
			pg_md5_init(&ctx->data.md5);
			break;
		case PG_SHA1:
			pg_sha1_init(&ctx->data.sha1);
			break;
		case PG_SHA224:
			pg_sha224_init(&ctx->data.sha224);
			break;
		case PG_SHA256:
			pg_sha256_init(&ctx->data.sha256);
			break;
		case PG_SHA384:
			pg_sha384_init(&ctx->data.sha384);
			break;
		case PG_SHA512:
			pg_sha512_init(&ctx->data.sha512);
			break;
	}

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
	if (ctx == NULL)
		return -1;

	switch (ctx->type)
	{
		case PG_MD5:
			pg_md5_update(&ctx->data.md5, data, len);
			break;
		case PG_SHA1:
			pg_sha1_update(&ctx->data.sha1, data, len);
			break;
		case PG_SHA224:
			pg_sha224_update(&ctx->data.sha224, data, len);
			break;
		case PG_SHA256:
			pg_sha256_update(&ctx->data.sha256, data, len);
			break;
		case PG_SHA384:
			pg_sha384_update(&ctx->data.sha384, data, len);
			break;
		case PG_SHA512:
			pg_sha512_update(&ctx->data.sha512, data, len);
			break;
	}

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
	if (ctx == NULL)
		return -1;

	switch (ctx->type)
	{
		case PG_MD5:
			if (len < MD5_DIGEST_LENGTH)
			{
				ctx->error = PG_CRYPTOHASH_ERROR_DEST_LEN;
				return -1;
			}
			pg_md5_final(&ctx->data.md5, dest);
			break;
		case PG_SHA1:
			if (len < SHA1_DIGEST_LENGTH)
			{
				ctx->error = PG_CRYPTOHASH_ERROR_DEST_LEN;
				return -1;
			}
			pg_sha1_final(&ctx->data.sha1, dest);
			break;
		case PG_SHA224:
			if (len < PG_SHA224_DIGEST_LENGTH)
			{
				ctx->error = PG_CRYPTOHASH_ERROR_DEST_LEN;
				return -1;
			}
			pg_sha224_final(&ctx->data.sha224, dest);
			break;
		case PG_SHA256:
			if (len < PG_SHA256_DIGEST_LENGTH)
			{
				ctx->error = PG_CRYPTOHASH_ERROR_DEST_LEN;
				return -1;
			}
			pg_sha256_final(&ctx->data.sha256, dest);
			break;
		case PG_SHA384:
			if (len < PG_SHA384_DIGEST_LENGTH)
			{
				ctx->error = PG_CRYPTOHASH_ERROR_DEST_LEN;
				return -1;
			}
			pg_sha384_final(&ctx->data.sha384, dest);
			break;
		case PG_SHA512:
			if (len < PG_SHA512_DIGEST_LENGTH)
			{
				ctx->error = PG_CRYPTOHASH_ERROR_DEST_LEN;
				return -1;
			}
			pg_sha512_final(&ctx->data.sha512, dest);
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

	explicit_bzero(ctx, sizeof(pg_cryptohash_ctx));
	FREE(ctx);
}

/*
 * pg_cryptohash_error
 *
 * Returns a static string providing details about an error that
 * happened during a computation.
 */
const char *
pg_cryptohash_error(pg_cryptohash_ctx *ctx)
{
	/*
	 * This implementation would never fail because of an out-of-memory error,
	 * except when creating the context.
	 */
	if (ctx == NULL)
		return _("out of memory");

	switch (ctx->error)
	{
		case PG_CRYPTOHASH_ERROR_NONE:
			return _("success");
		case PG_CRYPTOHASH_ERROR_DEST_LEN:
			return _("destination buffer too small");
	}

	Assert(false);
	return _("success");
}
