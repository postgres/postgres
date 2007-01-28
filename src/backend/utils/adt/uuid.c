/*-------------------------------------------------------------------------
 *
 * uuid.c
 *	  Functions for the built-in type "uuid".
 *
 * Copyright (c) 2007, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/uuid.c,v 1.1 2007/01/28 16:16:52 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/uuid.h"

/* Accepted GUID formats */

/* UUID_FMT1 is the default output format */
#define UUID_FMT1 "%02hhx%02hhx%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx"
#define UUID_FMT2 "{%02hhx%02hhx%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx}"
#define UUID_FMT3 "%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx"

/* UUIDs are accepted in any of the following textual input formats. */
#define UUID_CHK_FMT1 "00000000-0000-0000-0000-000000000000"
#define UUID_CHK_FMT2 "{00000000-0000-0000-0000-000000000000}"
#define UUID_CHK_FMT3 "00000000000000000000000000000000"

#define PRINT_SIZE 40

/* uuid size in bytes */
#define UUID_LEN 16

/* The uuid_t type is declared as struct uuid_t in uuid.h */
struct uuid_t
{
    char  data[UUID_LEN];
};

static void uuid_data_from_string(const char *source, unsigned char *data);
static void string_from_uuid_data(const char *fmt, const char *data, char *uuid_str);
static bool parse_uuid_string(const char *fmt, const char *chk_fmt,
							  const char *source, unsigned char *data);
static bool is_valid_format(const char *source, const char *fmt);
static int32 uuid_internal_cmp(uuid_t *arg1, uuid_t *arg2);

Datum
uuid_in(PG_FUNCTION_ARGS)
{
	char 		*uuid_str = PG_GETARG_CSTRING(0);
	uuid_t 		*uuid;
	uint8 		 data[UUID_LEN];

	uuid_data_from_string(uuid_str, data);
	uuid = (uuid_t *) palloc(sizeof(uuid_t));
	memcpy(uuid->data, data, UUID_LEN);
	PG_RETURN_UUID_P(uuid);
}

Datum
uuid_out(PG_FUNCTION_ARGS)
{
	uuid_t 		*uuid = (uuid_t *) PG_GETARG_POINTER(0);
	char 		*uuid_str;

	uuid_str = (char *) palloc(PRINT_SIZE);
	string_from_uuid_data(UUID_FMT1, uuid->data, uuid_str);
	PG_RETURN_CSTRING(uuid_str);
}

/* string to uuid convertor by various format types */
static void
uuid_data_from_string(const char *source, unsigned char *data)
{
	if (!parse_uuid_string(UUID_FMT1, UUID_CHK_FMT1, source, data) &&
		!parse_uuid_string(UUID_FMT2, UUID_CHK_FMT2, source, data) &&
		!parse_uuid_string(UUID_FMT3, UUID_CHK_FMT3, source, data))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for uuid: \"%s\"",
						source)));
	}
}

/* check the validity of a uuid string by a given format */
static bool
is_valid_format(const char *source, const char *fmt)
{
	int i;
	int fmtlen = strlen(fmt);

	/* check length first */
	if (fmtlen != strlen(source))
		return false;

	for (i = 0; i < fmtlen; i++)
	{
		int 	fc;
		int 	sc;
		bool 	valid_chr;

		fc = fmt[i];
		sc = source[i];

		/* false if format chr is { or - and source is not */
		if (fc != '0' && fc != sc)
			return false;

		/* check for valid char in source */
		valid_chr = (sc >= '0' && sc <= '9') ||
					(sc >= 'a' && sc <= 'f' ) ||
					(sc >= 'A' && sc <= 'F' );
		
		if (fc == '0' && !valid_chr)
			return false;
	}

	return true;
}

/* parse the uuid string to a format and return true if okay */
static bool
parse_uuid_string(const char *fmt, const char *chk_fmt,
				  const char *source, unsigned char *data)
{
	int result = sscanf(source, fmt,
						&data[0], &data[1], &data[2], &data[3], &data[4],
						&data[5], &data[6], &data[7], &data[8], &data[9],
						&data[10], &data[11], &data[12], &data[13],
						&data[14], &data[15]);

	return (result == 16) && is_valid_format(source, chk_fmt);
}

/* create a string representation of the uuid */
static void
string_from_uuid_data(const char *fmt, const char *data, char *uuid_str)
{
    snprintf(uuid_str, PRINT_SIZE, fmt,
			 data[0], data[1], data[2], data[3], data[4],
			 data[5], data[6], data[7], data[8], data[9],
			 data[10], data[11], data[12], data[13],
			 data[14], data[15]);
}

Datum
uuid_recv(PG_FUNCTION_ARGS)
{
	StringInfo 	 buffer = (StringInfo) PG_GETARG_POINTER(0);
	uuid_t 		*uuid;

	uuid = (uuid_t *) palloc(UUID_LEN);
	memcpy(uuid->data, pq_getmsgbytes(buffer, UUID_LEN), UUID_LEN);
	PG_RETURN_POINTER(uuid);
}

Datum
uuid_send(PG_FUNCTION_ARGS)
{
	uuid_t 				*uuid = PG_GETARG_UUID_P(0);
	StringInfoData 		 buffer;

	pq_begintypsend(&buffer);
	pq_sendbytes(&buffer, uuid->data, UUID_LEN);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buffer));
}

/* internal uuid compare function */
static int32
uuid_internal_cmp(uuid_t *arg1, uuid_t *arg2)
{
	return memcmp(arg1->data, arg2->data, UUID_LEN);
}

Datum
uuid_lt(PG_FUNCTION_ARGS)
{
	uuid_t 		*arg1 = PG_GETARG_UUID_P(0);
	uuid_t 		*arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) < 0);
}

Datum
uuid_le(PG_FUNCTION_ARGS)
{
	uuid_t 		*arg1 = PG_GETARG_UUID_P(0);
	uuid_t 		*arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) <= 0);
}

Datum
uuid_eq(PG_FUNCTION_ARGS)
{
	uuid_t 		*arg1 = PG_GETARG_UUID_P(0);
	uuid_t 		*arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) == 0);
}

Datum
uuid_ge(PG_FUNCTION_ARGS)
{
	uuid_t 		*arg1 = PG_GETARG_UUID_P(0);
	uuid_t 		*arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) >= 0);
}

Datum
uuid_gt(PG_FUNCTION_ARGS)
{
	uuid_t 		*arg1 = PG_GETARG_UUID_P(0);
	uuid_t 		*arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) > 0);
}

Datum
uuid_ne(PG_FUNCTION_ARGS)
{
	uuid_t 		*arg1 = PG_GETARG_UUID_P(0);
	uuid_t 		*arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) != 0);
}

/* handler for btree index operator */
Datum
uuid_cmp(PG_FUNCTION_ARGS)
{
	uuid_t 		*arg1 = PG_GETARG_UUID_P(0);
	uuid_t 		*arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_INT32(uuid_internal_cmp(arg1, arg2));
}

/* hash index support */
Datum
uuid_hash(PG_FUNCTION_ARGS)
{
	uuid_t	   *key = PG_GETARG_UUID_P(0);
	return hash_any((unsigned char *) key, sizeof(uuid_t));
}

/* cast text to uuid */
Datum
text_uuid(PG_FUNCTION_ARGS)
{
	text 		*input = PG_GETARG_TEXT_P(0);
	int		 	 length;
	char		*str;
	Datum	 	 result;

	length = VARSIZE(input) - VARHDRSZ;
	str = palloc(length + 1);
	memcpy(str, VARDATA(input), length);
	*(str + length) = '\0';

	result = DirectFunctionCall1(uuid_in, CStringGetDatum(str));
	pfree(str);
	PG_RETURN_DATUM(result);
}

/* cast uuid to text */
Datum
uuid_text(PG_FUNCTION_ARGS)
{
	uuid_t *uuid = PG_GETARG_UUID_P(0);
	Datum uuid_str = DirectFunctionCall1(uuid_out, UUIDPGetDatum(uuid));

	PG_RETURN_DATUM(DirectFunctionCall1(textin, uuid_str));
}
