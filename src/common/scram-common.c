/*-------------------------------------------------------------------------
 * scram-common.c
 *		Shared frontend/backend code for SCRAM authentication
 *
 * This contains the common low-level functions needed in both frontend and
 * backend, for implement the Salted Challenge Response Authentication
 * Mechanism (SCRAM), per IETF's RFC 5802.
 *
 * Portions Copyright (c) 2017, PostgreSQL Global Development Group
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

/* for htonl */
#include <netinet/in.h>
#include <arpa/inet.h>

#include "common/scram-common.h"

#define HMAC_IPAD 0x36
#define HMAC_OPAD 0x5C

/*
 * Calculate HMAC per RFC2104.
 *
 * The hash function used is SHA-256.
 */
void
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
		pg_sha256_ctx sha256_ctx;

		pg_sha256_init(&sha256_ctx);
		pg_sha256_update(&sha256_ctx, key, keylen);
		pg_sha256_final(&sha256_ctx, keybuf);
		key = keybuf;
		keylen = SCRAM_KEY_LEN;
	}

	memset(k_ipad, HMAC_IPAD, SHA256_HMAC_B);
	memset(ctx->k_opad, HMAC_OPAD, SHA256_HMAC_B);

	for (i = 0; i < keylen; i++)
	{
		k_ipad[i] ^= key[i];
		ctx->k_opad[i] ^= key[i];
	}

	/* tmp = H(K XOR ipad, text) */
	pg_sha256_init(&ctx->sha256ctx);
	pg_sha256_update(&ctx->sha256ctx, k_ipad, SHA256_HMAC_B);
}

/*
 * Update HMAC calculation
 * The hash function used is SHA-256.
 */
void
scram_HMAC_update(scram_HMAC_ctx *ctx, const char *str, int slen)
{
	pg_sha256_update(&ctx->sha256ctx, (const uint8 *) str, slen);
}

/*
 * Finalize HMAC calculation.
 * The hash function used is SHA-256.
 */
void
scram_HMAC_final(uint8 *result, scram_HMAC_ctx *ctx)
{
	uint8		h[SCRAM_KEY_LEN];

	pg_sha256_final(&ctx->sha256ctx, h);

	/* H(K XOR opad, tmp) */
	pg_sha256_init(&ctx->sha256ctx);
	pg_sha256_update(&ctx->sha256ctx, ctx->k_opad, SHA256_HMAC_B);
	pg_sha256_update(&ctx->sha256ctx, h, SCRAM_KEY_LEN);
	pg_sha256_final(&ctx->sha256ctx, result);
}

/*
 * Iterate hash calculation of HMAC entry using given salt.
 * scram_Hi() is essentially PBKDF2 (see RFC2898) with HMAC() as the
 * pseudorandom function.
 */
static void
scram_Hi(const char *str, const char *salt, int saltlen, int iterations, uint8 *result)
{
	int			str_len = strlen(str);
	uint32		one = htonl(1);
	int			i,
				j;
	uint8		Ui[SCRAM_KEY_LEN];
	uint8		Ui_prev[SCRAM_KEY_LEN];
	scram_HMAC_ctx hmac_ctx;

	/* First iteration */
	scram_HMAC_init(&hmac_ctx, (uint8 *) str, str_len);
	scram_HMAC_update(&hmac_ctx, salt, saltlen);
	scram_HMAC_update(&hmac_ctx, (char *) &one, sizeof(uint32));
	scram_HMAC_final(Ui_prev, &hmac_ctx);
	memcpy(result, Ui_prev, SCRAM_KEY_LEN);

	/* Subsequent iterations */
	for (i = 2; i <= iterations; i++)
	{
		scram_HMAC_init(&hmac_ctx, (uint8 *) str, str_len);
		scram_HMAC_update(&hmac_ctx, (const char *) Ui_prev, SCRAM_KEY_LEN);
		scram_HMAC_final(Ui, &hmac_ctx);
		for (j = 0; j < SCRAM_KEY_LEN; j++)
			result[j] ^= Ui[j];
		memcpy(Ui_prev, Ui, SCRAM_KEY_LEN);
	}
}


/*
 * Calculate SHA-256 hash for a NULL-terminated string. (The NULL terminator is
 * not included in the hash).
 */
void
scram_H(const uint8 *input, int len, uint8 *result)
{
	pg_sha256_ctx ctx;

	pg_sha256_init(&ctx);
	pg_sha256_update(&ctx, input, len);
	pg_sha256_final(&ctx, result);
}

/*
 * Encrypt password for SCRAM authentication. This basically applies the
 * normalization of the password and a hash calculation using the salt
 * value given by caller.
 */
static void
scram_SaltedPassword(const char *password, const char *salt, int saltlen, int iterations,
					 uint8 *result)
{
	/*
	 * XXX: Here SASLprep should be applied on password. However, per RFC5802,
	 * it is required that the password is encoded in UTF-8, something that is
	 * not guaranteed in this protocol. We may want to revisit this
	 * normalization function once encoding functions are available as well in
	 * the frontend in order to be able to encode properly this string, and
	 * then apply SASLprep on it.
	 */

	scram_Hi(password, salt, saltlen, iterations, result);
}

/*
 * Calculate ClientKey or ServerKey.
 */
void
scram_ClientOrServerKey(const char *password,
						const char *salt, int saltlen, int iterations,
						const char *keystr, uint8 *result)
{
	uint8		keybuf[SCRAM_KEY_LEN];
	scram_HMAC_ctx ctx;

	scram_SaltedPassword(password, salt, saltlen, iterations, keybuf);
	scram_HMAC_init(&ctx, keybuf, SCRAM_KEY_LEN);
	scram_HMAC_update(&ctx, keystr, strlen(keystr));
	scram_HMAC_final(result, &ctx);
}
