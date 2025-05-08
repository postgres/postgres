/*-------------------------------------------------------------------------
 * scram-common.c
 *		Shared frontend/backend code for SCRAM authentication
 *
 * This contains the common low-level functions needed in both frontend and
 * backend, for implement the Salted Challenge Response Authentication
 * Mechanism (SCRAM), per IETF's RFC 5802.
 *
 * Portions Copyright (c) 2017-2025, PostgreSQL Global Development Group
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
#include "common/hmac.h"
#include "common/scram-common.h"
#ifndef FRONTEND
#include "miscadmin.h"
#endif
#include "port/pg_bswap.h"

/*
 * Calculate SaltedPassword.
 *
 * The password should already be normalized by SASLprep.  Returns 0 on
 * success, -1 on failure with *errstr pointing to a message about the
 * error details.
 */
int
scram_SaltedPassword(const char *password,
					 pg_cryptohash_type hash_type, int key_length,
					 const uint8 *salt, int saltlen, int iterations,
					 uint8 *result, const char **errstr)
{
	int			password_len = strlen(password);
	uint32		one = pg_hton32(1);
	int			i,
				j;
	uint8		Ui[SCRAM_MAX_KEY_LEN];
	uint8		Ui_prev[SCRAM_MAX_KEY_LEN];
	pg_hmac_ctx *hmac_ctx = pg_hmac_create(hash_type);

	if (hmac_ctx == NULL)
	{
		*errstr = pg_hmac_error(NULL);	/* returns OOM */
		return -1;
	}

	/*
	 * Iterate hash calculation of HMAC entry using given salt.  This is
	 * essentially PBKDF2 (see RFC2898) with HMAC() as the pseudorandom
	 * function.
	 */

	/* First iteration */
	if (pg_hmac_init(hmac_ctx, (uint8 *) password, password_len) < 0 ||
		pg_hmac_update(hmac_ctx, salt, saltlen) < 0 ||
		pg_hmac_update(hmac_ctx, (uint8 *) &one, sizeof(uint32)) < 0 ||
		pg_hmac_final(hmac_ctx, Ui_prev, key_length) < 0)
	{
		*errstr = pg_hmac_error(hmac_ctx);
		pg_hmac_free(hmac_ctx);
		return -1;
	}

	memcpy(result, Ui_prev, key_length);

	/* Subsequent iterations */
	for (i = 1; i < iterations; i++)
	{
#ifndef FRONTEND
		/*
		 * Make sure that this is interruptible as scram_iterations could be
		 * set to a large value.
		 */
		CHECK_FOR_INTERRUPTS();
#endif

		if (pg_hmac_init(hmac_ctx, (uint8 *) password, password_len) < 0 ||
			pg_hmac_update(hmac_ctx, (uint8 *) Ui_prev, key_length) < 0 ||
			pg_hmac_final(hmac_ctx, Ui, key_length) < 0)
		{
			*errstr = pg_hmac_error(hmac_ctx);
			pg_hmac_free(hmac_ctx);
			return -1;
		}

		for (j = 0; j < key_length; j++)
			result[j] ^= Ui[j];
		memcpy(Ui_prev, Ui, key_length);
	}

	pg_hmac_free(hmac_ctx);
	return 0;
}


/*
 * Calculate hash for a NULL-terminated string. (The NULL terminator is
 * not included in the hash).  Returns 0 on success, -1 on failure with *errstr
 * pointing to a message about the error details.
 */
int
scram_H(const uint8 *input, pg_cryptohash_type hash_type, int key_length,
		uint8 *result, const char **errstr)
{
	pg_cryptohash_ctx *ctx;

	ctx = pg_cryptohash_create(hash_type);
	if (ctx == NULL)
	{
		*errstr = pg_cryptohash_error(NULL);	/* returns OOM */
		return -1;
	}

	if (pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, input, key_length) < 0 ||
		pg_cryptohash_final(ctx, result, key_length) < 0)
	{
		*errstr = pg_cryptohash_error(ctx);
		pg_cryptohash_free(ctx);
		return -1;
	}

	pg_cryptohash_free(ctx);
	return 0;
}

/*
 * Calculate ClientKey.  Returns 0 on success, -1 on failure with *errstr
 * pointing to a message about the error details.
 */
int
scram_ClientKey(const uint8 *salted_password,
				pg_cryptohash_type hash_type, int key_length,
				uint8 *result, const char **errstr)
{
	pg_hmac_ctx *ctx = pg_hmac_create(hash_type);

	if (ctx == NULL)
	{
		*errstr = pg_hmac_error(NULL);	/* returns OOM */
		return -1;
	}

	if (pg_hmac_init(ctx, salted_password, key_length) < 0 ||
		pg_hmac_update(ctx, (uint8 *) "Client Key", strlen("Client Key")) < 0 ||
		pg_hmac_final(ctx, result, key_length) < 0)
	{
		*errstr = pg_hmac_error(ctx);
		pg_hmac_free(ctx);
		return -1;
	}

	pg_hmac_free(ctx);
	return 0;
}

/*
 * Calculate ServerKey.  Returns 0 on success, -1 on failure with *errstr
 * pointing to a message about the error details.
 */
int
scram_ServerKey(const uint8 *salted_password,
				pg_cryptohash_type hash_type, int key_length,
				uint8 *result, const char **errstr)
{
	pg_hmac_ctx *ctx = pg_hmac_create(hash_type);

	if (ctx == NULL)
	{
		*errstr = pg_hmac_error(NULL);	/* returns OOM */
		return -1;
	}

	if (pg_hmac_init(ctx, salted_password, key_length) < 0 ||
		pg_hmac_update(ctx, (uint8 *) "Server Key", strlen("Server Key")) < 0 ||
		pg_hmac_final(ctx, result, key_length) < 0)
	{
		*errstr = pg_hmac_error(ctx);
		pg_hmac_free(ctx);
		return -1;
	}

	pg_hmac_free(ctx);
	return 0;
}


/*
 * Construct a SCRAM secret, for storing in pg_authid.rolpassword.
 *
 * The password should already have been processed with SASLprep, if necessary!
 *
 * The result is palloc'd or malloc'd, so caller is responsible for freeing it.
 *
 * On error, returns NULL and sets *errstr to point to a message about the
 * error details.
 */
char *
scram_build_secret(pg_cryptohash_type hash_type, int key_length,
				   const uint8 *salt, int saltlen, int iterations,
				   const char *password, const char **errstr)
{
	uint8		salted_password[SCRAM_MAX_KEY_LEN];
	uint8		stored_key[SCRAM_MAX_KEY_LEN];
	uint8		server_key[SCRAM_MAX_KEY_LEN];
	char	   *result;
	char	   *p;
	int			maxlen;
	int			encoded_salt_len;
	int			encoded_stored_len;
	int			encoded_server_len;
	int			encoded_result;

	/* Only this hash method is supported currently */
	Assert(hash_type == PG_SHA256);

	Assert(iterations > 0);

	/* Calculate StoredKey and ServerKey */
	if (scram_SaltedPassword(password, hash_type, key_length,
							 salt, saltlen, iterations,
							 salted_password, errstr) < 0 ||
		scram_ClientKey(salted_password, hash_type, key_length,
						stored_key, errstr) < 0 ||
		scram_H(stored_key, hash_type, key_length,
				stored_key, errstr) < 0 ||
		scram_ServerKey(salted_password, hash_type, key_length,
						server_key, errstr) < 0)
	{
		/* errstr is filled already here */
#ifdef FRONTEND
		return NULL;
#else
		elog(ERROR, "could not calculate stored key and server key: %s",
			 *errstr);
#endif
	}

	/*----------
	 * The format is:
	 * SCRAM-SHA-256$<iteration count>:<salt>$<StoredKey>:<ServerKey>
	 *----------
	 */
	encoded_salt_len = pg_b64_enc_len(saltlen);
	encoded_stored_len = pg_b64_enc_len(key_length);
	encoded_server_len = pg_b64_enc_len(key_length);

	maxlen = strlen("SCRAM-SHA-256") + 1
		+ 10 + 1				/* iteration count */
		+ encoded_salt_len + 1	/* Base64-encoded salt */
		+ encoded_stored_len + 1	/* Base64-encoded StoredKey */
		+ encoded_server_len + 1;	/* Base64-encoded ServerKey */

#ifdef FRONTEND
	result = malloc(maxlen);
	if (!result)
	{
		*errstr = _("out of memory");
		return NULL;
	}
#else
	result = palloc(maxlen);
#endif

	p = result + sprintf(result, "SCRAM-SHA-256$%d:", iterations);

	/* salt */
	encoded_result = pg_b64_encode(salt, saltlen, p, encoded_salt_len);
	if (encoded_result < 0)
	{
		*errstr = _("could not encode salt");
#ifdef FRONTEND
		free(result);
		return NULL;
#else
		elog(ERROR, "%s", *errstr);
#endif
	}
	p += encoded_result;
	*(p++) = '$';

	/* stored key */
	encoded_result = pg_b64_encode(stored_key, key_length, p,
								   encoded_stored_len);
	if (encoded_result < 0)
	{
		*errstr = _("could not encode stored key");
#ifdef FRONTEND
		free(result);
		return NULL;
#else
		elog(ERROR, "%s", *errstr);
#endif
	}

	p += encoded_result;
	*(p++) = ':';

	/* server key */
	encoded_result = pg_b64_encode(server_key, key_length, p,
								   encoded_server_len);
	if (encoded_result < 0)
	{
		*errstr = _("could not encode server key");
#ifdef FRONTEND
		free(result);
		return NULL;
#else
		elog(ERROR, "%s", *errstr);
#endif
	}

	p += encoded_result;
	*(p++) = '\0';

	Assert(p - result <= maxlen);

	return result;
}
