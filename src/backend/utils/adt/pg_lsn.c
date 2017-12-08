/*-------------------------------------------------------------------------
 *
 * pg_lsn.c
 *	  Operations for the pg_lsn datatype.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pg_lsn.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

#define MAXPG_LSNLEN			17
#define MAXPG_LSNCOMPONENT	8

/*----------------------------------------------------------
 * Formatting and conversion routines.
 *---------------------------------------------------------*/

Datum
pg_lsn_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	int			len1,
				len2;
	uint32		id,
				off;
	XLogRecPtr	result;

	/* Sanity check input format. */
	len1 = strspn(str, "0123456789abcdefABCDEF");
	if (len1 < 1 || len1 > MAXPG_LSNCOMPONENT || str[len1] != '/')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"pg_lsn", str)));
	len2 = strspn(str + len1 + 1, "0123456789abcdefABCDEF");
	if (len2 < 1 || len2 > MAXPG_LSNCOMPONENT || str[len1 + 1 + len2] != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"pg_lsn", str)));

	/* Decode result. */
	id = (uint32) strtoul(str, NULL, 16);
	off = (uint32) strtoul(str + len1 + 1, NULL, 16);
	result = ((uint64) id << 32) | off;

	PG_RETURN_LSN(result);
}

Datum
pg_lsn_out(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn = PG_GETARG_LSN(0);
	char		buf[MAXPG_LSNLEN + 1];
	char	   *result;
	uint32		id,
				off;

	/* Decode ID and offset */
	id = (uint32) (lsn >> 32);
	off = (uint32) lsn;

	snprintf(buf, sizeof buf, "%X/%X", id, off);
	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

Datum
pg_lsn_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	XLogRecPtr	result;

	result = pq_getmsgint64(buf);
	PG_RETURN_LSN(result);
}

Datum
pg_lsn_send(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn = PG_GETARG_LSN(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, lsn);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*----------------------------------------------------------
 *	Operators for PostgreSQL LSNs
 *---------------------------------------------------------*/

Datum
pg_lsn_eq(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(lsn1 == lsn2);
}

Datum
pg_lsn_ne(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(lsn1 != lsn2);
}

Datum
pg_lsn_lt(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(lsn1 < lsn2);
}

Datum
pg_lsn_gt(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(lsn1 > lsn2);
}

Datum
pg_lsn_le(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(lsn1 <= lsn2);
}

Datum
pg_lsn_ge(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(lsn1 >= lsn2);
}

/* btree index opclass support */
Datum
pg_lsn_cmp(PG_FUNCTION_ARGS)
{
	XLogRecPtr	a = PG_GETARG_LSN(0);
	XLogRecPtr	b = PG_GETARG_LSN(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

/* hash index opclass support */
Datum
pg_lsn_hash(PG_FUNCTION_ARGS)
{
	/* We can use hashint8 directly */
	return hashint8(fcinfo);
}

Datum
pg_lsn_hash_extended(PG_FUNCTION_ARGS)
{
	return hashint8extended(fcinfo);
}


/*----------------------------------------------------------
 *	Arithmetic operators on PostgreSQL LSNs.
 *---------------------------------------------------------*/

Datum
pg_lsn_mi(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);
	char		buf[256];
	Datum		result;

	/* Output could be as large as plus or minus 2^63 - 1. */
	if (lsn1 < lsn2)
		snprintf(buf, sizeof buf, "-" UINT64_FORMAT, lsn2 - lsn1);
	else
		snprintf(buf, sizeof buf, UINT64_FORMAT, lsn1 - lsn2);

	/* Convert to numeric. */
	result = DirectFunctionCall3(numeric_in,
								 CStringGetDatum(buf),
								 ObjectIdGetDatum(0),
								 Int32GetDatum(-1));

	return result;
}
