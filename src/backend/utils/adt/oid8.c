/*-------------------------------------------------------------------------
 *
 * oid8.c
 *	  Functions for the built-in type Oid8
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/oid8.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"

#define MAXOID8LEN 20

/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

Datum
oid8in(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
	Oid8		result;

	result = uint64in_subr(s, NULL, "oid8", fcinfo->context);
	PG_RETURN_OID8(result);
}

Datum
oid8out(PG_FUNCTION_ARGS)
{
	Oid8		val = PG_GETARG_OID8(0);
	char		buf[MAXOID8LEN + 1];
	char	   *result;
	int			len;

	len = pg_ulltoa_n(val, buf) + 1;
	buf[len - 1] = '\0';

	/*
	 * Since the length is already known, we do a manual palloc() and memcpy()
	 * to avoid the strlen() call that would otherwise be done in pstrdup().
	 */
	result = palloc(len);
	memcpy(result, buf, len);
	PG_RETURN_CSTRING(result);
}

/*
 *		oid8recv			- converts external binary format to oid8
 */
Datum
oid8recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_OID8(pq_getmsgint64(buf));
}

/*
 *		oid8send			- converts oid8 to binary format
 */
Datum
oid8send(PG_FUNCTION_ARGS)
{
	Oid8		arg1 = PG_GETARG_OID8(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

Datum
oid8eq(PG_FUNCTION_ARGS)
{
	Oid8		arg1 = PG_GETARG_OID8(0);
	Oid8		arg2 = PG_GETARG_OID8(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
oid8ne(PG_FUNCTION_ARGS)
{
	Oid8		arg1 = PG_GETARG_OID8(0);
	Oid8		arg2 = PG_GETARG_OID8(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
oid8lt(PG_FUNCTION_ARGS)
{
	Oid8		arg1 = PG_GETARG_OID8(0);
	Oid8		arg2 = PG_GETARG_OID8(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
oid8le(PG_FUNCTION_ARGS)
{
	Oid8		arg1 = PG_GETARG_OID8(0);
	Oid8		arg2 = PG_GETARG_OID8(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
oid8ge(PG_FUNCTION_ARGS)
{
	Oid8		arg1 = PG_GETARG_OID8(0);
	Oid8		arg2 = PG_GETARG_OID8(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}

Datum
oid8gt(PG_FUNCTION_ARGS)
{
	Oid8		arg1 = PG_GETARG_OID8(0);
	Oid8		arg2 = PG_GETARG_OID8(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
hashoid8(PG_FUNCTION_ARGS)
{
	return hashint8(fcinfo);
}

Datum
hashoid8extended(PG_FUNCTION_ARGS)
{
	return hashint8extended(fcinfo);
}

Datum
oid8larger(PG_FUNCTION_ARGS)
{
	Oid8		arg1 = PG_GETARG_OID8(0);
	Oid8		arg2 = PG_GETARG_OID8(1);

	PG_RETURN_OID8((arg1 > arg2) ? arg1 : arg2);
}

Datum
oid8smaller(PG_FUNCTION_ARGS)
{
	Oid8		arg1 = PG_GETARG_OID8(0);
	Oid8		arg2 = PG_GETARG_OID8(1);

	PG_RETURN_OID8((arg1 < arg2) ? arg1 : arg2);
}
