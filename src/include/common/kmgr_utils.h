/*-------------------------------------------------------------------------
 *
 * kmgr_utils.h
 *		Declarations for utility function for file encryption key
 *
 * Portions Copyright (c) 2020, PostgreSQL Global Development Group
 *
 * src/include/common/kmgr_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef KMGR_UTILS_H
#define KMGR_UTILS_H

#include "common/cipher.h"

/* Current version number */
#define KMGR_VERSION 1

/*
 * Directories where cluster file encryption keys reside within PGDATA.
 */
#define KMGR_DIR			"pg_cryptokeys"
#define KMGR_DIR_PID		KMGR_DIR"/pg_alterckey.pid"
#define LIVE_KMGR_DIR		KMGR_DIR"/live"
/* used during cluster key rotation */
#define NEW_KMGR_DIR		KMGR_DIR"/new"
#define OLD_KMGR_DIR		KMGR_DIR"/old"

/* CryptoKey file name is keys id */
#define CryptoKeyFilePath(path, dir, id) \
	snprintf((path), MAXPGPATH, "%s/%d", (dir), (id))

/*
 * Identifiers of internal keys.
 */
#define KMGR_KEY_ID_REL 		0
#define KMGR_KEY_ID_WAL 		1
#define KMGR_MAX_INTERNAL_KEYS	2

/* We always, today, use a 256-bit AES key. */
#define KMGR_CLUSTER_KEY_LEN 	PG_AES256_KEY_LEN

/* double for hex format, plus some for spaces, \r,\n, and null byte */
#define ALLOC_KMGR_CLUSTER_KEY_LEN	(KMGR_CLUSTER_KEY_LEN * 2 + 10 + 2 + 1)

/* Maximum length of key the key manager can store */
#define KMGR_MAX_KEY_LEN			256
#define KMGR_MAX_KEY_LEN_BYTES		KMGR_MAX_KEY_LEN / 8
#define KMGR_MAX_WRAPPED_KEY_LEN	KmgrSizeOfCipherText(KMGR_MAX_KEY_LEN)


/*
 * Cryptographic key data structure.
 *
 * This is the structure we use to write out the encrypted keys.
 *
 * pgkey_id is the identifier for this key (should be same as the
 * file name and be one of KMGR_KEY_ID_* from above).  This is what
 * we consider our 'context' or 'fixed' portion of the deterministic
 * IV we create.
 *
 * counter is updated each time we use the cluster KEK to encrypt a
 * new key.  This is our the 'invocation' field of the deterministic
 * IV we create.
 *
 * Absolutely essential when using GCM (or CTR) is that the IV is unique,
 * for a given key, but a deterministic IV such as this is perfectly
 * acceptable and encouraged.  If (and only if!) the KEK is changed to a
 * new key, then we can re-initialize the counter.
 *
 * Detailed discussion of deterministic IV creation can be found here:
 *
 * https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38d.pdf
 *
 * tag is the GCM tag which is produced and must be validated in order
 * to be able to trust the results of our decryption.
 *
 * encrypted_key is the encrypted key length (as an int) + encrypted key.
 */
typedef struct CryptoKey
{
	uint64	pgkey_id;								/* Upper half of IV */
	uint64	counter;								/* Lower half of IV */
	unsigned char tag[16];							/* GCM tag */
	unsigned char encrypted_key[sizeof(int) + KMGR_MAX_KEY_LEN_BYTES];
} CryptoKey;

extern bool kmgr_wrap_key(PgCipherCtx *ctx, CryptoKey *in, CryptoKey *out);
extern bool kmgr_unwrap_key(PgCipherCtx *ctx, CryptoKey *in, CryptoKey *out);
extern bool kmgr_verify_cluster_key(unsigned char *cluster_key,
								    CryptoKey *in_keys, CryptoKey *out_keys,
									int nkey);
extern int	kmgr_run_cluster_key_command(char *cluster_key_command,
												char *buf, int size, char *dir);
extern CryptoKey *kmgr_get_cryptokeys(const char *path, int *nkeys);

#endif							/* KMGR_UTILS_H */
