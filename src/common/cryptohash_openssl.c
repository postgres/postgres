/*-------------------------------------------------------------------------
 *
 * cryptohash_openssl.c
 *	  Set of wrapper routines on top of OpenSSL to support cryptographic
 *	  hash functions.
 *
 * This should only be used if code is compiled with OpenSSL support.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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

#include <openssl/err.h>
#include <openssl/evp.h>

#include "common/cryptohash.h"
#include "common/md5.h"
#include "common/sha1.h"
#include "common/sha2.h"
#ifndef FRONTEND
#include "utils/memutils.h"
#include "utils/resowner.h"
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

/* Set of error states */
typedef enum pg_cryptohash_errno
{
	PG_CRYPTOHASH_ERROR_NONE = 0,
	PG_CRYPTOHASH_ERROR_DEST_LEN,
	PG_CRYPTOHASH_ERROR_OPENSSL,
} pg_cryptohash_errno;

/*
 * Internal pg_cryptohash_ctx structure.
 *
 * This tracks the resource owner associated to each EVP context data
 * for the backend.
 */
struct pg_cryptohash_ctx
{
	pg_cryptohash_type type;
	pg_cryptohash_errno error;
	const char *errreason;

	EVP_MD_CTX *evpctx;

#ifndef FRONTEND
	ResourceOwner resowner;
#endif
};

/* ResourceOwner callbacks to hold cryptohash contexts */
#ifndef FRONTEND
static void ResOwnerReleaseCryptoHash(Datum res);

static const ResourceOwnerDesc cryptohash_resowner_desc =
{
	.name = "OpenSSL cryptohash context",
	.release_phase = RESOURCE_RELEASE_BEFORE_LOCKS,
	.release_priority = RELEASE_PRIO_CRYPTOHASH_CONTEXTS,
	.ReleaseResource = ResOwnerReleaseCryptoHash,
	.DebugPrint = NULL			/* the default message is fine */
};

/* Convenience wrappers over ResourceOwnerRemember/Forget */
static inline void
ResourceOwnerRememberCryptoHash(ResourceOwner owner, pg_cryptohash_ctx *ctx)
{
	ResourceOwnerRemember(owner, PointerGetDatum(ctx), &cryptohash_resowner_desc);
}
static inline void
ResourceOwnerForgetCryptoHash(ResourceOwner owner, pg_cryptohash_ctx *ctx)
{
	ResourceOwnerForget(owner, PointerGetDatum(ctx), &cryptohash_resowner_desc);
}
#endif

static const char *
SSLerrmessage(unsigned long ecode)
{
	if (ecode == 0)
		return NULL;

	/*
	 * This may return NULL, but we would fall back to a default error path if
	 * that were the case.
	 */
	return ERR_reason_error_string(ecode);
}

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
	ResourceOwnerEnlarge(CurrentResourceOwner);
#endif

	ctx = ALLOC(sizeof(pg_cryptohash_ctx));
	if (ctx == NULL)
		return NULL;
	memset(ctx, 0, sizeof(pg_cryptohash_ctx));
	ctx->type = type;
	ctx->error = PG_CRYPTOHASH_ERROR_NONE;
	ctx->errreason = NULL;

	/*
	 * Initialization takes care of assigning the correct type for OpenSSL.
	 * Also ensure that there aren't any unconsumed errors in the queue from
	 * previous runs.
	 */
	ERR_clear_error();
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
	ResourceOwnerRememberCryptoHash(CurrentResourceOwner, ctx);
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
	{
		ctx->errreason = SSLerrmessage(ERR_get_error());
		ctx->error = PG_CRYPTOHASH_ERROR_OPENSSL;

		/*
		 * The OpenSSL error queue should normally be empty since we've
		 * consumed an error, but cipher initialization can in FIPS-enabled
		 * OpenSSL builds generate two errors so clear the queue here as well.
		 */
		ERR_clear_error();
		return -1;
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
	int			status = 0;

	if (ctx == NULL)
		return -1;

	status = EVP_DigestUpdate(ctx->evpctx, data, len);

	/* OpenSSL internals return 1 on success, 0 on failure */
	if (status <= 0)
	{
		ctx->errreason = SSLerrmessage(ERR_get_error());
		ctx->error = PG_CRYPTOHASH_ERROR_OPENSSL;
		return -1;
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
	int			status = 0;

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
			break;
		case PG_SHA1:
			if (len < SHA1_DIGEST_LENGTH)
			{
				ctx->error = PG_CRYPTOHASH_ERROR_DEST_LEN;
				return -1;
			}
			break;
		case PG_SHA224:
			if (len < PG_SHA224_DIGEST_LENGTH)
			{
				ctx->error = PG_CRYPTOHASH_ERROR_DEST_LEN;
				return -1;
			}
			break;
		case PG_SHA256:
			if (len < PG_SHA256_DIGEST_LENGTH)
			{
				ctx->error = PG_CRYPTOHASH_ERROR_DEST_LEN;
				return -1;
			}
			break;
		case PG_SHA384:
			if (len < PG_SHA384_DIGEST_LENGTH)
			{
				ctx->error = PG_CRYPTOHASH_ERROR_DEST_LEN;
				return -1;
			}
			break;
		case PG_SHA512:
			if (len < PG_SHA512_DIGEST_LENGTH)
			{
				ctx->error = PG_CRYPTOHASH_ERROR_DEST_LEN;
				return -1;
			}
			break;
	}

	status = EVP_DigestFinal_ex(ctx->evpctx, dest, 0);

	/* OpenSSL internals return 1 on success, 0 on failure */
	if (status <= 0)
	{
		ctx->errreason = SSLerrmessage(ERR_get_error());
		ctx->error = PG_CRYPTOHASH_ERROR_OPENSSL;
		return -1;
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

	EVP_MD_CTX_destroy(ctx->evpctx);

#ifndef FRONTEND
	if (ctx->resowner)
		ResourceOwnerForgetCryptoHash(ctx->resowner, ctx);
#endif

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

	/*
	 * If a reason is provided, rely on it, else fallback to any error code
	 * set.
	 */
	if (ctx->errreason)
		return ctx->errreason;

	switch (ctx->error)
	{
		case PG_CRYPTOHASH_ERROR_NONE:
			return _("success");
		case PG_CRYPTOHASH_ERROR_DEST_LEN:
			return _("destination buffer too small");
		case PG_CRYPTOHASH_ERROR_OPENSSL:
			return _("OpenSSL failure");
	}

	Assert(false);				/* cannot be reached */
	return _("success");
}

/* ResourceOwner callbacks */

#ifndef FRONTEND
static void
ResOwnerReleaseCryptoHash(Datum res)
{
	pg_cryptohash_ctx *ctx = (pg_cryptohash_ctx *) DatumGetPointer(res);

	ctx->resowner = NULL;
	pg_cryptohash_free(ctx);
}
#endif
