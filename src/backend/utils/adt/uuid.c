/*-------------------------------------------------------------------------
 *
 * uuid.c
 *	  Functions for the built-in type "uuid".
 *
 * Copyright (c) 2007-2008, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/uuid.c,v 1.7 2008/01/01 20:31:21 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/uuid.h"

/* uuid size in bytes */
#define UUID_LEN 16

/* pg_uuid_t is declared to be struct pg_uuid_t in uuid.h */
struct pg_uuid_t
{
	unsigned char data[UUID_LEN];
};

static void string_to_uuid(const char *source, pg_uuid_t *uuid);
static int	uuid_internal_cmp(const pg_uuid_t *arg1, const pg_uuid_t *arg2);

Datum
uuid_in(PG_FUNCTION_ARGS)
{
	char	   *uuid_str = PG_GETARG_CSTRING(0);
	pg_uuid_t  *uuid;

	uuid = (pg_uuid_t *) palloc(sizeof(*uuid));
	string_to_uuid(uuid_str, uuid);
	PG_RETURN_UUID_P(uuid);
}

Datum
uuid_out(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *uuid = PG_GETARG_UUID_P(0);
	static const char hex_chars[] = "0123456789abcdef";
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);
	for (i = 0; i < UUID_LEN; i++)
	{
		int			hi;
		int			lo;

		/*
		 * We print uuid values as a string of 8, 4, 4, 4, and then 12
		 * hexadecimal characters, with each group is separated by a hyphen
		 * ("-"). Therefore, add the hyphens at the appropriate places here.
		 */
		if (i == 4 || i == 6 || i == 8 || i == 10)
			appendStringInfoChar(&buf, '-');

		hi = uuid->data[i] >> 4;
		lo = uuid->data[i] & 0x0F;

		appendStringInfoChar(&buf, hex_chars[hi]);
		appendStringInfoChar(&buf, hex_chars[lo]);
	}

	PG_RETURN_CSTRING(buf.data);
}

/*
 * We allow UUIDs in three input formats: 8x-4x-4x-4x-12x,
 * {8x-4x-4x-4x-12x}, and 32x, where "nx" means n hexadecimal digits
 * (only the first format is used for output). We convert the first
 * two formats into the latter format before further processing.
 */
static void
string_to_uuid(const char *source, pg_uuid_t *uuid)
{
	char		hex_buf[32];	/* not NUL terminated */
	int			i;
	int			src_len;

	src_len = strlen(source);
	if (src_len != 32 && src_len != 36 && src_len != 38)
		goto syntax_error;

	if (src_len == 32)
		memcpy(hex_buf, source, src_len);
	else
	{
		const char *str = source;

		if (src_len == 38)
		{
			if (str[0] != '{' || str[37] != '}')
				goto syntax_error;

			str++;				/* skip the first character */
		}

		if (str[8] != '-' || str[13] != '-' ||
			str[18] != '-' || str[23] != '-')
			goto syntax_error;

		memcpy(hex_buf, str, 8);
		memcpy(hex_buf + 8, str + 9, 4);
		memcpy(hex_buf + 12, str + 14, 4);
		memcpy(hex_buf + 16, str + 19, 4);
		memcpy(hex_buf + 20, str + 24, 12);
	}

	for (i = 0; i < UUID_LEN; i++)
	{
		char		str_buf[3];

		memcpy(str_buf, &hex_buf[i * 2], 2);
		if (!isxdigit((unsigned char) str_buf[0]) ||
			!isxdigit((unsigned char) str_buf[1]))
			goto syntax_error;

		str_buf[2] = '\0';
		uuid->data[i] = (unsigned char) strtoul(str_buf, NULL, 16);
	}

	return;

syntax_error:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for uuid: \"%s\"",
					source)));
}

Datum
uuid_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buffer = (StringInfo) PG_GETARG_POINTER(0);
	pg_uuid_t  *uuid;

	uuid = (pg_uuid_t *) palloc(UUID_LEN);
	memcpy(uuid->data, pq_getmsgbytes(buffer, UUID_LEN), UUID_LEN);
	PG_RETURN_POINTER(uuid);
}

Datum
uuid_send(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *uuid = PG_GETARG_UUID_P(0);
	StringInfoData buffer;

	pq_begintypsend(&buffer);
	pq_sendbytes(&buffer, (char *) uuid->data, UUID_LEN);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buffer));
}

/* internal uuid compare function */
static int
uuid_internal_cmp(const pg_uuid_t *arg1, const pg_uuid_t *arg2)
{
	return memcmp(arg1->data, arg2->data, UUID_LEN);
}

Datum
uuid_lt(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) < 0);
}

Datum
uuid_le(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) <= 0);
}

Datum
uuid_eq(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) == 0);
}

Datum
uuid_ge(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) >= 0);
}

Datum
uuid_gt(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) > 0);
}

Datum
uuid_ne(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) != 0);
}

/* handler for btree index operator */
Datum
uuid_cmp(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_INT32(uuid_internal_cmp(arg1, arg2));
}

/* hash index support */
Datum
uuid_hash(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *key = PG_GETARG_UUID_P(0);

	return hash_any(key->data, UUID_LEN);
}
