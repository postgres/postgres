/*-------------------------------------------------------------------------
 *
 * scram-common.h
 *		Declarations for helper functions used for SCRAM authentication
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/scram-common.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SCRAM_COMMON_H
#define SCRAM_COMMON_H

#include "common/sha2.h"

/* Length of SCRAM keys (client and server) */
#define SCRAM_KEY_LEN				PG_SHA256_DIGEST_LENGTH

/* length of HMAC */
#define SHA256_HMAC_B				PG_SHA256_BLOCK_LENGTH

/*
 * Size of random nonce generated in the authentication exchange.  This
 * is in "raw" number of bytes, the actual nonces sent over the wire are
 * encoded using only ASCII-printable characters.
 */
#define SCRAM_RAW_NONCE_LEN			10

/* length of salt when generating new verifiers */
#define SCRAM_SALT_LEN				10

/* number of bytes used when sending iteration number during exchange */
#define SCRAM_ITERATION_LEN			10

/* default number of iterations when generating verifier */
#define SCRAM_ITERATIONS_DEFAULT	4096

/* Base name of keys used for proof generation */
#define SCRAM_SERVER_KEY_NAME "Server Key"
#define SCRAM_CLIENT_KEY_NAME "Client Key"

/*
 * Context data for HMAC used in SCRAM authentication.
 */
typedef struct
{
	pg_sha256_ctx sha256ctx;
	uint8		k_opad[SHA256_HMAC_B];
} scram_HMAC_ctx;

extern void scram_HMAC_init(scram_HMAC_ctx *ctx, const uint8 *key, int keylen);
extern void scram_HMAC_update(scram_HMAC_ctx *ctx, const char *str, int slen);
extern void scram_HMAC_final(uint8 *result, scram_HMAC_ctx *ctx);

extern void scram_H(const uint8 *str, int len, uint8 *result);
extern void scram_ClientOrServerKey(const char *password, const char *salt,
						int saltlen, int iterations,
						const char *keystr, uint8 *result);

#endif   /* SCRAM_COMMON_H */
