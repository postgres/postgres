/*-------------------------------------------------------------------------
 *
 * cryptohashes.c
 *	  Cryptographic hash functions
 *
 * Portions Copyright (c) 2018-2020, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/cryptohashes.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/md5.h"
#include "common/sha2.h"
#include "utils/builtins.h"


/*
 * MD5
 */

/* MD5 produces a 16 byte (128 bit) hash; double it for hex */
#define MD5_HASH_LEN  32

/*
 * Create an MD5 hash of a text value and return it as hex string.
 */
Datum
md5_text(PG_FUNCTION_ARGS)
{
	text	   *in_text = PG_GETARG_TEXT_PP(0);
	size_t		len;
	char		hexsum[MD5_HASH_LEN + 1];

	/* Calculate the length of the buffer using varlena metadata */
	len = VARSIZE_ANY_EXHDR(in_text);

	/* get the hash result */
	if (pg_md5_hash(VARDATA_ANY(in_text), len, hexsum) == false)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* convert to text and return it */
	PG_RETURN_TEXT_P(cstring_to_text(hexsum));
}

/*
 * Create an MD5 hash of a bytea value and return it as a hex string.
 */
Datum
md5_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_PP(0);
	size_t		len;
	char		hexsum[MD5_HASH_LEN + 1];

	len = VARSIZE_ANY_EXHDR(in);
	if (pg_md5_hash(VARDATA_ANY(in), len, hexsum) == false)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	PG_RETURN_TEXT_P(cstring_to_text(hexsum));
}


/*
 * SHA-2 variants
 */

Datum
sha224_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_PP(0);
	const uint8 *data;
	size_t		len;
	pg_sha224_ctx ctx;
	unsigned char buf[PG_SHA224_DIGEST_LENGTH];
	bytea	   *result;

	len = VARSIZE_ANY_EXHDR(in);
	data = (unsigned char *) VARDATA_ANY(in);

	pg_sha224_init(&ctx);
	pg_sha224_update(&ctx, data, len);
	pg_sha224_final(&ctx, buf);

	result = palloc(sizeof(buf) + VARHDRSZ);
	SET_VARSIZE(result, sizeof(buf) + VARHDRSZ);
	memcpy(VARDATA(result), buf, sizeof(buf));

	PG_RETURN_BYTEA_P(result);
}

Datum
sha256_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_PP(0);
	const uint8 *data;
	size_t		len;
	pg_sha256_ctx ctx;
	unsigned char buf[PG_SHA256_DIGEST_LENGTH];
	bytea	   *result;

	len = VARSIZE_ANY_EXHDR(in);
	data = (unsigned char *) VARDATA_ANY(in);

	pg_sha256_init(&ctx);
	pg_sha256_update(&ctx, data, len);
	pg_sha256_final(&ctx, buf);

	result = palloc(sizeof(buf) + VARHDRSZ);
	SET_VARSIZE(result, sizeof(buf) + VARHDRSZ);
	memcpy(VARDATA(result), buf, sizeof(buf));

	PG_RETURN_BYTEA_P(result);
}

Datum
sha384_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_PP(0);
	const uint8 *data;
	size_t		len;
	pg_sha384_ctx ctx;
	unsigned char buf[PG_SHA384_DIGEST_LENGTH];
	bytea	   *result;

	len = VARSIZE_ANY_EXHDR(in);
	data = (unsigned char *) VARDATA_ANY(in);

	pg_sha384_init(&ctx);
	pg_sha384_update(&ctx, data, len);
	pg_sha384_final(&ctx, buf);

	result = palloc(sizeof(buf) + VARHDRSZ);
	SET_VARSIZE(result, sizeof(buf) + VARHDRSZ);
	memcpy(VARDATA(result), buf, sizeof(buf));

	PG_RETURN_BYTEA_P(result);
}

Datum
sha512_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_PP(0);
	const uint8 *data;
	size_t		len;
	pg_sha512_ctx ctx;
	unsigned char buf[PG_SHA512_DIGEST_LENGTH];
	bytea	   *result;

	len = VARSIZE_ANY_EXHDR(in);
	data = (unsigned char *) VARDATA_ANY(in);

	pg_sha512_init(&ctx);
	pg_sha512_update(&ctx, data, len);
	pg_sha512_final(&ctx, buf);

	result = palloc(sizeof(buf) + VARHDRSZ);
	SET_VARSIZE(result, sizeof(buf) + VARHDRSZ);
	memcpy(VARDATA(result), buf, sizeof(buf));

	PG_RETURN_BYTEA_P(result);
}
