/*-------------------------------------------------------------------------
 *
 * checksum_helper.c
 *	  Compute a checksum of any of various types using common routines
 *
 * Portions Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/common/checksum_helper.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/checksum_helper.h"

/*
 * If 'name' is a recognized checksum type, set *type to the corresponding
 * constant and return true. Otherwise, set *type to CHECKSUM_TYPE_NONE and
 * return false.
 */
bool
pg_checksum_parse_type(char *name, pg_checksum_type *type)
{
	pg_checksum_type result_type = CHECKSUM_TYPE_NONE;
	bool		result = true;

	if (pg_strcasecmp(name, "none") == 0)
		result_type = CHECKSUM_TYPE_NONE;
	else if (pg_strcasecmp(name, "crc32c") == 0)
		result_type = CHECKSUM_TYPE_CRC32C;
	else if (pg_strcasecmp(name, "sha224") == 0)
		result_type = CHECKSUM_TYPE_SHA224;
	else if (pg_strcasecmp(name, "sha256") == 0)
		result_type = CHECKSUM_TYPE_SHA256;
	else if (pg_strcasecmp(name, "sha384") == 0)
		result_type = CHECKSUM_TYPE_SHA384;
	else if (pg_strcasecmp(name, "sha512") == 0)
		result_type = CHECKSUM_TYPE_SHA512;
	else
		result = false;

	*type = result_type;
	return result;
}

/*
 * Get the canonical human-readable name corresponding to a checksum type.
 */
char *
pg_checksum_type_name(pg_checksum_type type)
{
	switch (type)
	{
		case CHECKSUM_TYPE_NONE:
			return "NONE";
		case CHECKSUM_TYPE_CRC32C:
			return "CRC32C";
		case CHECKSUM_TYPE_SHA224:
			return "SHA224";
		case CHECKSUM_TYPE_SHA256:
			return "SHA256";
		case CHECKSUM_TYPE_SHA384:
			return "SHA384";
		case CHECKSUM_TYPE_SHA512:
			return "SHA512";
	}

	Assert(false);
	return "???";
}

/*
 * Initialize a checksum context for checksums of the given type.
 * Returns 0 for a success, -1 for a failure.
 */
int
pg_checksum_init(pg_checksum_context *context, pg_checksum_type type)
{
	context->type = type;

	switch (type)
	{
		case CHECKSUM_TYPE_NONE:
			/* do nothing */
			break;
		case CHECKSUM_TYPE_CRC32C:
			INIT_CRC32C(context->raw_context.c_crc32c);
			break;
		case CHECKSUM_TYPE_SHA224:
			context->raw_context.c_sha2 = pg_cryptohash_create(PG_SHA224);
			if (context->raw_context.c_sha2 == NULL)
				return -1;
			if (pg_cryptohash_init(context->raw_context.c_sha2) < 0)
			{
				pg_cryptohash_free(context->raw_context.c_sha2);
				return -1;
			}
			break;
		case CHECKSUM_TYPE_SHA256:
			context->raw_context.c_sha2 = pg_cryptohash_create(PG_SHA256);
			if (context->raw_context.c_sha2 == NULL)
				return -1;
			if (pg_cryptohash_init(context->raw_context.c_sha2) < 0)
			{
				pg_cryptohash_free(context->raw_context.c_sha2);
				return -1;
			}
			break;
		case CHECKSUM_TYPE_SHA384:
			context->raw_context.c_sha2 = pg_cryptohash_create(PG_SHA384);
			if (context->raw_context.c_sha2 == NULL)
				return -1;
			if (pg_cryptohash_init(context->raw_context.c_sha2) < 0)
			{
				pg_cryptohash_free(context->raw_context.c_sha2);
				return -1;
			}
			break;
		case CHECKSUM_TYPE_SHA512:
			context->raw_context.c_sha2 = pg_cryptohash_create(PG_SHA512);
			if (context->raw_context.c_sha2 == NULL)
				return -1;
			if (pg_cryptohash_init(context->raw_context.c_sha2) < 0)
			{
				pg_cryptohash_free(context->raw_context.c_sha2);
				return -1;
			}
			break;
	}

	return 0;
}

/*
 * Update a checksum context with new data.
 * Returns 0 for a success, -1 for a failure.
 */
int
pg_checksum_update(pg_checksum_context *context, const uint8 *input,
				   size_t len)
{
	switch (context->type)
	{
		case CHECKSUM_TYPE_NONE:
			/* do nothing */
			break;
		case CHECKSUM_TYPE_CRC32C:
			COMP_CRC32C(context->raw_context.c_crc32c, input, len);
			break;
		case CHECKSUM_TYPE_SHA224:
		case CHECKSUM_TYPE_SHA256:
		case CHECKSUM_TYPE_SHA384:
		case CHECKSUM_TYPE_SHA512:
			if (pg_cryptohash_update(context->raw_context.c_sha2, input, len) < 0)
				return -1;
			break;
	}

	return 0;
}

/*
 * Finalize a checksum computation and write the result to an output buffer.
 *
 * The caller must ensure that the buffer is at least PG_CHECKSUM_MAX_LENGTH
 * bytes in length. The return value is the number of bytes actually written,
 * or -1 for a failure.
 */
int
pg_checksum_final(pg_checksum_context *context, uint8 *output)
{
	int			retval = 0;

	StaticAssertDecl(sizeof(pg_crc32c) <= PG_CHECKSUM_MAX_LENGTH,
					 "CRC-32C digest too big for PG_CHECKSUM_MAX_LENGTH");
	StaticAssertDecl(PG_SHA224_DIGEST_LENGTH <= PG_CHECKSUM_MAX_LENGTH,
					 "SHA224 digest too big for PG_CHECKSUM_MAX_LENGTH");
	StaticAssertDecl(PG_SHA256_DIGEST_LENGTH <= PG_CHECKSUM_MAX_LENGTH,
					 "SHA256 digest too big for PG_CHECKSUM_MAX_LENGTH");
	StaticAssertDecl(PG_SHA384_DIGEST_LENGTH <= PG_CHECKSUM_MAX_LENGTH,
					 "SHA384 digest too big for PG_CHECKSUM_MAX_LENGTH");
	StaticAssertDecl(PG_SHA512_DIGEST_LENGTH <= PG_CHECKSUM_MAX_LENGTH,
					 "SHA512 digest too big for PG_CHECKSUM_MAX_LENGTH");

	switch (context->type)
	{
		case CHECKSUM_TYPE_NONE:
			break;
		case CHECKSUM_TYPE_CRC32C:
			FIN_CRC32C(context->raw_context.c_crc32c);
			retval = sizeof(pg_crc32c);
			memcpy(output, &context->raw_context.c_crc32c, retval);
			break;
		case CHECKSUM_TYPE_SHA224:
			retval = PG_SHA224_DIGEST_LENGTH;
			if (pg_cryptohash_final(context->raw_context.c_sha2,
									output, retval) < 0)
				return -1;
			pg_cryptohash_free(context->raw_context.c_sha2);
			break;
		case CHECKSUM_TYPE_SHA256:
			retval = PG_SHA256_DIGEST_LENGTH;
			if (pg_cryptohash_final(context->raw_context.c_sha2,
									output, retval) < 0)
				return -1;
			pg_cryptohash_free(context->raw_context.c_sha2);
			break;
		case CHECKSUM_TYPE_SHA384:
			retval = PG_SHA384_DIGEST_LENGTH;
			if (pg_cryptohash_final(context->raw_context.c_sha2,
									output, retval) < 0)
				return -1;
			pg_cryptohash_free(context->raw_context.c_sha2);
			break;
		case CHECKSUM_TYPE_SHA512:
			retval = PG_SHA512_DIGEST_LENGTH;
			if (pg_cryptohash_final(context->raw_context.c_sha2,
									output, retval) < 0)
				return -1;
			pg_cryptohash_free(context->raw_context.c_sha2);
			break;
	}

	Assert(retval <= PG_CHECKSUM_MAX_LENGTH);
	return retval;
}
