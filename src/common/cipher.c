/*-------------------------------------------------------------------------
 *
 * cipher.c
 *	  Shared frontend/backend for cryptographic functions
 *
 * Copyright (c) 2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/cipher.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/cipher.h"

static void cipher_failure(void) pg_attribute_noreturn();


PgCipherCtx *
pg_cipher_ctx_create(int cipher, uint8 *key, int klen, bool enc)
{
	cipher_failure();
	return NULL;				/* keep compiler quiet */
}

void
pg_cipher_ctx_free(PgCipherCtx *ctx)
{
	cipher_failure();
}

bool
pg_cipher_encrypt(PgCipherCtx *ctx, const unsigned char *plaintext,
				  const int inlen, unsigned char *ciphertext, int *outlen,
				  const unsigned char *iv, const int ivlen,
				  unsigned char *outtag, const int taglen)
{
	cipher_failure();
	return false;				/* keep compiler quiet */
}

bool
pg_cipher_decrypt(PgCipherCtx *ctx, const unsigned char *ciphertext,
				  const int inlen, unsigned char *plaintext, int *outlen,
				  const unsigned char *iv, const int ivlen,
				  unsigned char *intag, const int taglen)
{
	cipher_failure();
	return false;				/* keep compiler quiet */
}

static void
cipher_failure(void)
{
#ifndef FRONTEND
	ereport(ERROR,
			(errcode(ERRCODE_CONFIG_FILE_ERROR),
			 (errmsg("cluster file encryption is not supported because OpenSSL is not supported by this build"),
			  errhint("Compile with --with-openssl to use this feature."))));
#else
	fprintf(stderr, _("cluster file encryption is not supported because OpenSSL is not supported by this build"));
	exit(1);
#endif
}
