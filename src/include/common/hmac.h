/*-------------------------------------------------------------------------
 *
 * hmac.h
 *	  Generic headers for HMAC
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/include/common/hmac.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_HMAC_H
#define PG_HMAC_H

#include "common/cryptohash.h"

/* opaque context, private to each HMAC implementation */
typedef struct pg_hmac_ctx pg_hmac_ctx;

extern pg_hmac_ctx *pg_hmac_create(pg_cryptohash_type type);
extern int	pg_hmac_init(pg_hmac_ctx *ctx, const uint8 *key, size_t len);
extern int	pg_hmac_update(pg_hmac_ctx *ctx, const uint8 *data, size_t len);
extern int	pg_hmac_final(pg_hmac_ctx *ctx, uint8 *dest, size_t len);
extern void pg_hmac_free(pg_hmac_ctx *ctx);
extern const char *pg_hmac_error(pg_hmac_ctx *ctx);

#endif							/* PG_HMAC_H */
