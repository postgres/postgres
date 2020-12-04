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

#include <openssl/evp.h>

#include "common/cryptohash.h"
#ifndef FRONTEND
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/resowner_private.h"
#endif

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
 * Internal structure for pg_cryptohash_ctx->data.
 *
 * This tracks the resource owner associated to each EVP context data
 * for the backend.
 */
typedef struct pg_cryptohash_state
{
	EVP_MD_CTX *evpctx;

#ifndef FRONTEND
	ResourceOwner resowner;
#endif
} pg_cryptohash_state;

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
	pg_cryptohash_state *state;

	ctx = ALLOC(sizeof(pg_cryptohash_ctx));
	if (ctx == NULL)
		return NULL;

	state = ALLOC(sizeof(pg_cryptohash_state));
	if (state == NULL)
	{
		explicit_bzero(ctx, sizeof(pg_cryptohash_ctx));
		FREE(ctx);
		return NULL;
	}

	ctx->data = state;
	ctx->type = type;

#ifndef FRONTEND
	ResourceOwnerEnlargeCryptoHash(CurrentResourceOwner);
#endif

	/*
	 * Initialization takes care of assigning the correct type for OpenSSL.
	 */
	state->evpctx = EVP_MD_CTX_create();

	if (state->evpctx == NULL)
	{
		explicit_bzero(state, sizeof(pg_cryptohash_state));
		explicit_bzero(ctx, sizeof(pg_cryptohash_ctx));
#ifndef FRONTEND
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
#else
		FREE(state);
		FREE(ctx);
		return NULL;
#endif
	}

#ifndef FRONTEND
	state->resowner = CurrentResourceOwner;
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
	pg_cryptohash_state *state;

	if (ctx == NULL)
		return 0;

	state = (pg_cryptohash_state *) ctx->data;

	switch (ctx->type)
	{
		case PG_SHA224:
			status = EVP_DigestInit_ex(state->evpctx, EVP_sha224(), NULL);
			break;
		case PG_SHA256:
			status = EVP_DigestInit_ex(state->evpctx, EVP_sha256(), NULL);
			break;
		case PG_SHA384:
			status = EVP_DigestInit_ex(state->evpctx, EVP_sha384(), NULL);
			break;
		case PG_SHA512:
			status = EVP_DigestInit_ex(state->evpctx, EVP_sha512(), NULL);
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
	pg_cryptohash_state *state;

	if (ctx == NULL)
		return 0;

	state = (pg_cryptohash_state *) ctx->data;
	status = EVP_DigestUpdate(state->evpctx, data, len);

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
	pg_cryptohash_state *state;

	if (ctx == NULL)
		return 0;

	state = (pg_cryptohash_state *) ctx->data;
	status = EVP_DigestFinal_ex(state->evpctx, dest, 0);

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
	pg_cryptohash_state *state;

	if (ctx == NULL)
		return;

	state = (pg_cryptohash_state *) ctx->data;
	EVP_MD_CTX_destroy(state->evpctx);

#ifndef FRONTEND
	ResourceOwnerForgetCryptoHash(state->resowner,
								  PointerGetDatum(ctx));
#endif

	explicit_bzero(state, sizeof(pg_cryptohash_state));
	explicit_bzero(ctx, sizeof(pg_cryptohash_ctx));
	FREE(state);
	FREE(ctx);
}
