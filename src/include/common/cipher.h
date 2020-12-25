/*-------------------------------------------------------------------------
 *
 * cipher.h
 *		Declarations for cryptographic functions
 *
 * Portions Copyright (c) 2020, PostgreSQL Global Development Group
 *
 * src/include/common/cipher.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CIPHER_H
#define PG_CIPHER_H

#ifdef USE_OPENSSL
#include <openssl/evp.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#endif

/*
 * Supported symmetric encryption algorithm. These identifiers are passed
 * to pg_cipher_ctx_create() function, and then actual encryption
 * implementations need to initialize their context of the given encryption
 * algorithm.
 */
#define PG_CIPHER_AES_GCM			0
#define PG_MAX_CIPHER_ID			1

/* AES128/192/256 various length definitions */
#define PG_AES128_KEY_LEN			(128 / 8)
#define PG_AES192_KEY_LEN			(192 / 8)
#define PG_AES256_KEY_LEN			(256 / 8)

/*
 * The encrypted data is a series of blocks of size. Initialization
 * vector(IV) is the same size of cipher block.
 */
#define PG_AES_BLOCK_SIZE			16
#define PG_AES_IV_SIZE				(PG_AES_BLOCK_SIZE)

#ifdef USE_OPENSSL
typedef EVP_CIPHER_CTX PgCipherCtx;
#else
typedef void PgCipherCtx;
#endif

extern PgCipherCtx *pg_cipher_ctx_create(int cipher, uint8 *key, int klen,
										 bool enc);
extern void pg_cipher_ctx_free(PgCipherCtx *ctx);
extern bool pg_cipher_encrypt(PgCipherCtx *ctx,
							  const unsigned char *plaintext, const int inlen,
							  unsigned char *ciphertext, int *outlen,
							  const unsigned char *iv, const int ivlen,
							  unsigned char *tag, const int taglen);
extern bool pg_cipher_decrypt(PgCipherCtx *ctx,
							  const unsigned char *ciphertext, const int inlen,
							  unsigned char *plaintext, int *outlen,
							  const unsigned char *iv, const int ivlen,
							  unsigned char *intag, const int taglen);

#endif							/* PG_CIPHER_H */
