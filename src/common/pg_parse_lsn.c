/*-------------------------------------------------------------------------
 *
 * pg_parse_lsn.c
 *	  Parse a WAL location (LSN) in its text form.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/pg_parse_lsn.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/pg_parse_lsn.h"

#define MAXPG_LSNCOMPONENT	8

/*
 * pg_parse_lsn
 *
 * Parse a WAL location in the "%X/%X" text form used for pg_lsn values,
 * requiring one to eight hexadecimal digits in each component and nothing
 * else.  Unlike sscanf(), this rejects components longer than eight
 * hexadecimal digits, leading whitespace, signs, "0x" prefixes, and
 * trailing characters.
 *
 * Returns true and sets *result on success; returns false on syntax
 * error, leaving *result unchanged.
 */
bool
pg_parse_lsn(const char *str, XLogRecPtr *result)
{
	size_t		len1,
				len2;

	len1 = strspn(str, "0123456789abcdefABCDEF");
	if (len1 < 1 || len1 > MAXPG_LSNCOMPONENT || str[len1] != '/')
		return false;

	len2 = strspn(str + len1 + 1, "0123456789abcdefABCDEF");
	if (len2 < 1 || len2 > MAXPG_LSNCOMPONENT || str[len1 + 1 + len2] != '\0')
		return false;

	*result = ((uint64) strtoul(str, NULL, 16)) << 32 |
		(uint32) strtoul(str + len1 + 1, NULL, 16);

	return true;
}
