/*-------------------------------------------------------------------------
 *
 * checksum_helper.h
 *	  Compute a checksum of any of various types using common routines
 *
 * Portions Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/include/common/checksum_helper.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef CHECKSUM_HELPER_H
#define CHECKSUM_HELPER_H

#include "common/cryptohash.h"
#include "common/sha2.h"
#include "port/pg_crc32c.h"

/*
 * Supported checksum types. It's not necessarily the case that code using
 * these functions needs a cryptographically strong checksum; it may only
 * need to detect accidental modification. That's why we include CRC-32C: it's
 * much faster than any of the other algorithms. On the other hand, we omit
 * MD5 here because any new that does need a cryptographically strong checksum
 * should use something better.
 */
typedef enum pg_checksum_type
{
	CHECKSUM_TYPE_NONE,
	CHECKSUM_TYPE_CRC32C,
	CHECKSUM_TYPE_SHA224,
	CHECKSUM_TYPE_SHA256,
	CHECKSUM_TYPE_SHA384,
	CHECKSUM_TYPE_SHA512
} pg_checksum_type;

/*
 * This is just a union of all applicable context types.
 */
typedef union pg_checksum_raw_context
{
	pg_crc32c	c_crc32c;
	pg_cryptohash_ctx *c_sha2;
} pg_checksum_raw_context;

/*
 * This structure provides a convenient way to pass the checksum type and the
 * checksum context around together.
 */
typedef struct pg_checksum_context
{
	pg_checksum_type type;
	pg_checksum_raw_context raw_context;
} pg_checksum_context;

/*
 * This is the longest possible output for any checksum algorithm supported
 * by this file.
 */
#define PG_CHECKSUM_MAX_LENGTH		PG_SHA512_DIGEST_LENGTH

extern bool pg_checksum_parse_type(char *name, pg_checksum_type *);
extern char *pg_checksum_type_name(pg_checksum_type);

extern int	pg_checksum_init(pg_checksum_context *, pg_checksum_type);
extern int	pg_checksum_update(pg_checksum_context *, const uint8 *input,
							   size_t len);
extern int	pg_checksum_final(pg_checksum_context *, uint8 *output);

#endif
