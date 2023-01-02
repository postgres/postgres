/*-------------------------------------------------------------------------
 *
 * cryptohash.h
 *	  Generic headers for cryptographic hash functions.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/include/common/cryptohash.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_CRYPTOHASH_H
#define PG_CRYPTOHASH_H

/* Context Structures for each hash function */
typedef enum
{
	PG_MD5 = 0,
	PG_SHA1,
	PG_SHA224,
	PG_SHA256,
	PG_SHA384,
	PG_SHA512
} pg_cryptohash_type;

/* opaque context, private to each cryptohash implementation */
typedef struct pg_cryptohash_ctx pg_cryptohash_ctx;

extern pg_cryptohash_ctx *pg_cryptohash_create(pg_cryptohash_type type);
extern int	pg_cryptohash_init(pg_cryptohash_ctx *ctx);
extern int	pg_cryptohash_update(pg_cryptohash_ctx *ctx, const uint8 *data, size_t len);
extern int	pg_cryptohash_final(pg_cryptohash_ctx *ctx, uint8 *dest, size_t len);
extern void pg_cryptohash_free(pg_cryptohash_ctx *ctx);
extern const char *pg_cryptohash_error(pg_cryptohash_ctx *ctx);

#endif							/* PG_CRYPTOHASH_H */
