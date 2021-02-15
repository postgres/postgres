/*-------------------------------------------------------------------------
 * scram-common.c
 *		Shared frontend/backend code for SCRAM authentication
 *
 * This contains the common low-level functions needed in both frontend and
 * backend, for implement the Salted Challenge Response Authentication
 * Mechanism (SCRAM), per IETF's RFC 5802.
 *
 * Portions Copyright (c) 2017-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/scram-common.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/base64.h"
#include "common/scram-common.h"
#include "port/pg_bswap.h"

#define HMAC_IPAD 0x36
#define HMAC_OPAD 0x5C

/*
 * Calculate HMAC per RFC2104.
 *
 * The hash function used is SHA-256.  Returns 0 on success, -1 on failure.
 */
int
scram_HMAC_init(scram_HMAC_ctx *ctx, const uint8 *key, int keylen)
{
	uint8		k_ipad[SHA256_HMAC_B];
	int			i;
	uint8		keybuf[SCRAM_KEY_LEN];

	/*
	 * If the key is longer than the block size (64 bytes for SHA-256), pass
	 * it through SHA-256 once to shrink it down.
	 */
	if (keylen > SHA256_HMAC_B)
	{
		pg_cryptohash_ctx *sha256_ctx;

		sha256_ctx = pg_cryptohash_create(PG_SHA256);
		if (sha256_ctx == NULL)
			return -1;
		if (pg_cryptohash_init(sha256_ctx) < 0 ||
			pg_cryptohash_update(sha256_ctx, key, keylen) < 0 ||
			pg_cryptohash_final(sha256_ctx, keybuf, sizeof(keybuf)) < 0)
		{
			pg_cryptohash_free(sha256_ctx);
			return -1;
		}
		key = keybuf;
		keylen = SCRAM_KEY_LEN;
		pg_cryptohash_free(sha256_ctx);
	}

	memset(k_ipad, HMAC_IPAD, SHA256_HMAC_B);
	memset(ctx->k_opad, HMAC_OPAD, SHA256_HMAC_B);

	for (i = 0; i < keylen; i++)
	{
		k_ipad[i] ^= key[i];
		ctx->k_opad[i] ^= key[i];
	}

	ctx->sha256ctx = pg_cryptohash_create(PG_SHA256);
	if (ctx->sha256ctx == NULL)
		return -1;

	/* tmp = H(K XOR ipad, text) */
	if (pg_cryptohash_init(ctx->sha256ctx) < 0 ||
		pg_cryptohash_update(ctx->sha256ctx, k_ipad, SHA256_HMAC_B) < 0)
	{
		pg_cryptohash_free(ctx->sha256ctx);
		return -1;
	}

	return 0;
}

/*
 * Update HMAC calculation
 * The hash function used is SHA-256.  Returns 0 on success, -1 on failure.
 */
int
scram_HMAC_update(scram_HMAC_ctx *ctx, const char *str, int slen)
{
	Assert(ctx->sha256ctx != NULL);
	if (pg_cryptohash_update(ctx->sha256ctx, (const uint8 *) str, slen) < 0)
	{
		pg_cryptohash_free(ctx->sha256ctx);
		return -1;
	}
	return 0;
}

/*
 * Finalize HMAC calculation.
 * The hash function used is SHA-256.  Returns 0 on success, -1 on failure.
 */
int
scram_HMAC_final(uint8 *result, scram_HMAC_ctx *ctx)
{
	uint8		h[SCRAM_KEY_LEN];

	Assert(ctx->sha256ctx != NULL);

	if (pg_cryptohash_final(ctx->sha256ctx, h, sizeof(h)) < 0)
	{
		pg_cryptohash_free(ctx->sha256ctx);
		return -1;
	}

	/* H(K XOR opad, tmp) */
	if (pg_cryptohash_init(ctx->sha256ctx) < 0 ||
		pg_cryptohash_update(ctx->sha256ctx, ctx->k_opad, SHA256_HMAC_B) < 0 ||
		pg_cryptohash_update(ctx->sha256ctx, h, SCRAM_KEY_LEN) < 0 ||
		pg_cryptohash_final(ctx->sha256ctx, result, SCRAM_KEY_LEN) < 0)
	{
		pg_cryptohash_free(ctx->sha256ctx);
		return -1;
	}

	pg_cryptohash_free(ctx->sha256ctx);
	return 0;
}

/*
 * Calculate SaltedPassword.
 *
 * The password should already be normalized by SASLprep.  Returns 0 on
 * success, -1 on failure.
 */
int
scram_SaltedPassword(const char *password,
					 const char *salt, int saltlen, int iterations,
					 uint8 *result)
{
	int			password_len = strlen(password);
	uint32		one = pg_hton32(1);
	int			i,
				j;
	uint8		Ui[SCRAM_KEY_LEN];
	uint8		Ui_prev[SCRAM_KEY_LEN];
	scram_HMAC_ctx hmac_ctx;

	/*
	 * Iterate hash calculation of HMAC entry using given salt.  This is
	 * essentially PBKDF2 (see RFC2898) with HMAC() as the pseudorandom
	 * function.
	 */

	/* First iteration */
	if (scram_HMAC_init(&hmac_ctx, (uint8 *) password, password_len) < 0 ||
		scram_HMAC_update(&hmac_ctx, salt, saltlen) < 0 ||
		scram_HMAC_update(&hmac_ctx, (char *) &one, sizeof(uint32)) < 0 ||
		scram_HMAC_final(Ui_prev, &hmac_ctx) < 0)
	{
		return -1;
	}

	memcpy(result, Ui_prev, SCRAM_KEY_LEN);

	/* Subsequent iterations */
	for (i = 2; i <= iterations; i++)
	{
		if (scram_HMAC_init(&hmac_ctx, (uint8 *) password, password_len) < 0 ||
			scram_HMAC_update(&hmac_ctx, (const char *) Ui_prev, SCRAM_KEY_LEN) < 0 ||
			scram_HMAC_final(Ui, &hmac_ctx) < 0)
		{
			return -1;
		}

		for (j = 0; j < SCRAM_KEY_LEN; j++)
			result[j] ^= Ui[j];
		memcpy(Ui_prev, Ui, SCRAM_KEY_LEN);
	}

	return 0;
}


/*
 * Calculate SHA-256 hash for a NULL-terminated string. (The NULL terminator is
 * not included in the hash).  Returns 0 on success, -1 on failure.
 */
int
scram_H(const uint8 *input, int len, uint8 *result)
{
	pg_cryptohash_ctx *ctx;

	ctx = pg_cryptohash_create(PG_SHA256);
	if (ctx == NULL)
		return -1;

	if (pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, input, len) < 0 ||
		pg_cryptohash_final(ctx, result, SCRAM_KEY_LEN) < 0)
	{
		pg_cryptohash_free(ctx);
		return -1;
	}

	pg_cryptohash_free(ctx);
	return 0;
}

/*
 * Calculate ClientKey.  Returns 0 on success, -1 on failure.
 */
int
scram_ClientKey(const uint8 *salted_password, uint8 *result)
{
	scram_HMAC_ctx ctx;

	if (scram_HMAC_init(&ctx, salted_password, SCRAM_KEY_LEN) < 0 ||
		scram_HMAC_update(&ctx, "Client Key", strlen("Client Key")) < 0 ||
		scram_HMAC_final(result, &ctx) < 0)
	{
		return -1;
	}

	return 0;
}

/*
 * Calculate ServerKey.  Returns 0 on success, -1 on failure.
 */
int
scram_ServerKey(const uint8 *salted_password, uint8 *result)
{
	scram_HMAC_ctx ctx;

	if (scram_HMAC_init(&ctx, salted_password, SCRAM_KEY_LEN) < 0 ||
		scram_HMAC_update(&ctx, "Server Key", strlen("Server Key")) < 0 ||
		scram_HMAC_final(result, &ctx) < 0)
	{
		return -1;
	}

	return 0;
}


/*
 * Construct a SCRAM secret, for storing in pg_authid.rolpassword.
 *
 * The password should already have been processed with SASLprep, if necessary!
 *
 * If iterations is 0, default number of iterations is used.  The result is
 * palloc'd or malloc'd, so caller is responsible for freeing it.
 */
char *
scram_build_secret(const char *salt, int saltlen, int iterations,
				   const char *password)
{
	uint8		salted_password[SCRAM_KEY_LEN];
	uint8		stored_key[SCRAM_KEY_LEN];
	uint8		server_key[SCRAM_KEY_LEN];
	char	   *result;
	char	   *p;
	int			maxlen;
	int			encoded_salt_len;
	int			encoded_stored_len;
	int			encoded_server_len;
	int			encoded_result;

	if (iterations <= 0)
		iterations = SCRAM_DEFAULT_ITERATIONS;

	/* Calculate StoredKey and ServerKey */
	if (scram_SaltedPassword(password, salt, saltlen, iterations,
							 salted_password) < 0 ||
		scram_ClientKey(salted_password, stored_key) < 0 ||
		scram_H(stored_key, SCRAM_KEY_LEN, stored_key) < 0 ||
		scram_ServerKey(salted_password, server_key) < 0)
	{
#ifdef FRONTEND
		return NULL;
#else
		elog(ERROR, "could not calculate stored key and server key");
#endif
	}

	/*----------
	 * The format is:
	 * SCRAM-SHA-256$<iteration count>:<salt>$<StoredKey>:<ServerKey>
	 *----------
	 */
	encoded_salt_len = pg_b64_enc_len(saltlen);
	encoded_stored_len = pg_b64_enc_len(SCRAM_KEY_LEN);
	encoded_server_len = pg_b64_enc_len(SCRAM_KEY_LEN);

	maxlen = strlen("SCRAM-SHA-256") + 1
		+ 10 + 1				/* iteration count */
		+ encoded_salt_len + 1	/* Base64-encoded salt */
		+ encoded_stored_len + 1	/* Base64-encoded StoredKey */
		+ encoded_server_len + 1;	/* Base64-encoded ServerKey */

#ifdef FRONTEND
	result = malloc(maxlen);
	if (!result)
		return NULL;
#else
	result = palloc(maxlen);
#endif

	p = result + sprintf(result, "SCRAM-SHA-256$%d:", iterations);

	/* salt */
	encoded_result = pg_b64_encode(salt, saltlen, p, encoded_salt_len);
	if (encoded_result < 0)
	{
#ifdef FRONTEND
		free(result);
		return NULL;
#else
		elog(ERROR, "could not encode salt");
#endif
	}
	p += encoded_result;
	*(p++) = '$';

	/* stored key */
	encoded_result = pg_b64_encode((char *) stored_key, SCRAM_KEY_LEN, p,
								   encoded_stored_len);
	if (encoded_result < 0)
	{
#ifdef FRONTEND
		free(result);
		return NULL;
#else
		elog(ERROR, "could not encode stored key");
#endif
	}

	p += encoded_result;
	*(p++) = ':';

	/* server key */
	encoded_result = pg_b64_encode((char *) server_key, SCRAM_KEY_LEN, p,
								   encoded_server_len);
	if (encoded_result < 0)
	{
#ifdef FRONTEND
		free(result);
		return NULL;
#else
		elog(ERROR, "could not encode server key");
#endif
	}

	p += encoded_result;
	*(p++) = '\0';

	Assert(p - result <= maxlen);

	return result;
}
