/*-------------------------------------------------------------------------
 *
 * int8.c
 *	  Internal 64-bit integer operations
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/int8.c,v 1.48 2003/09/25 06:58:04 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "libpq/pqformat.h"
#include "utils/int8.h"


#define MAXINT8LEN		25


/***********************************************************************
 **
 **		Routines for 64-bit integers.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * Formatting and conversion routines.
 *---------------------------------------------------------*/

/*
 * scanint8 --- try to parse a string into an int8.
 *
 * If errorOK is false, ereport a useful error message if the string is bad.
 * If errorOK is true, just return "false" for bad input.
 */
bool
scanint8(const char *str, bool errorOK, int64 *result)
{
	const char *ptr = str;
	int64		tmp = 0;
	int			sign = 1;

	/*
	 * Do our own scan, rather than relying on sscanf which might be
	 * broken for long long.
	 */

	/* skip leading spaces */
	while (*ptr && isspace((unsigned char) *ptr))
		ptr++;

	/* handle sign */
	if (*ptr == '-')
	{
		ptr++;
		sign = -1;

		/*
		 * Do an explicit check for INT64_MIN.	Ugly though this is, it's
		 * cleaner than trying to get the loop below to handle it
		 * portably.
		 */
#ifndef INT64_IS_BUSTED
		if (strcmp(ptr, "9223372036854775808") == 0)
		{
			*result = -INT64CONST(0x7fffffffffffffff) - 1;
			return true;
		}
#endif
	}
	else if (*ptr == '+')
		ptr++;

	/* require at least one digit */
	if (!isdigit((unsigned char) *ptr))
	{
		if (errorOK)
			return false;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				  errmsg("invalid input syntax for type bigint: \"%s\"", str)));
	}

	/* process digits */
	while (*ptr && isdigit((unsigned char) *ptr))
	{
		int64		newtmp = tmp * 10 + (*ptr++ - '0');

		if ((newtmp / 10) != tmp)		/* overflow? */
		{
			if (errorOK)
				return false;
			else
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("integer out of range")));
		}
		tmp = newtmp;
	}

	/* trailing junk? */
	if (*ptr)
	{
		if (errorOK)
			return false;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				  errmsg("invalid input syntax for type bigint: \"%s\"", str)));
	}

	*result = (sign < 0) ? -tmp : tmp;

	return true;
}

/* int8in()
 */
Datum
int8in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	int64		result;

	(void) scanint8(str, false, &result);
	PG_RETURN_INT64(result);
}


/* int8out()
 */
Datum
int8out(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	char	   *result;
	int			len;
	char		buf[MAXINT8LEN + 1];

	if ((len = snprintf(buf, MAXINT8LEN, INT64_FORMAT, val)) < 0)
		elog(ERROR, "could not format int8");

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		int8recv			- converts external binary format to int8
 */
Datum
int8recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INT64(pq_getmsgint64(buf));
}

/*
 *		int8send			- converts int8 to binary format
 */
Datum
int8send(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*----------------------------------------------------------
 *	Relational operators for int8s, including cross-data-type comparisons.
 *---------------------------------------------------------*/

/* int8relop()
 * Is val1 relop val2?
 */
Datum
int8eq(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int8ne(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int8lt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int8gt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int8le(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int8ge(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/* int84relop()
 * Is 64-bit val1 relop 32-bit val2?
 */
Datum
int84eq(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int84ne(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int84lt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int84gt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int84le(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int84ge(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/* int48relop()
 * Is 32-bit val1 relop 64-bit val2?
 */
Datum
int48eq(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int48ne(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int48lt(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int48gt(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int48le(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int48ge(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/* int82relop()
 * Is 64-bit val1 relop 16-bit val2?
 */
Datum
int82eq(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int82ne(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int82lt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int82gt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int82le(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int82ge(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/* int28relop()
 * Is 16-bit val1 relop 64-bit val2?
 */
Datum
int28eq(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int28ne(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int28lt(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int28gt(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int28le(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int28ge(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 >= val2);
}


/*----------------------------------------------------------
 *	Arithmetic operators on 64-bit integers.
 *---------------------------------------------------------*/

Datum
int8um(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);

	PG_RETURN_INT64(-val);
}

Datum
int8up(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);

	PG_RETURN_INT64(val);
}

Datum
int8pl(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(val1 + val2);
}

Datum
int8mi(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(val1 - val2);
}

Datum
int8mul(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(val1 * val2);
}

Datum
int8div(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	if (val2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	PG_RETURN_INT64(val1 / val2);
}

/* int8abs()
 * Absolute value
 */
Datum
int8abs(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);

	PG_RETURN_INT64((arg1 < 0) ? -arg1 : arg1);
}

/* int8mod()
 * Modulo operation.
 */
Datum
int8mod(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);
	int64		result;

	if (val2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = val1 / val2;
	result *= val2;
	result = val1 - result;

	PG_RETURN_INT64(result);
}

/* int8fac()
 * Factorial
 */
Datum
int8fac(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		result;
	int64		i;

	if (arg1 == 0)
		result = 1;
	else if (arg1 < 1)
		result = 0;
	else
		for (i = arg1, result = 1; i > 0; --i)
			result *= i;

	PG_RETURN_INT64(result);
}

Datum
int8inc(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);

	PG_RETURN_INT64(arg + 1);
}

Datum
int8larger(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);
	int64		result;

	result = ((val1 > val2) ? val1 : val2);

	PG_RETURN_INT64(result);
}

Datum
int8smaller(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);
	int64		result;

	result = ((val1 < val2) ? val1 : val2);

	PG_RETURN_INT64(result);
}

Datum
int84pl(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_INT64(val1 + val2);
}

Datum
int84mi(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_INT64(val1 - val2);
}

Datum
int84mul(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_INT64(val1 * val2);
}

Datum
int84div(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	if (val2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	PG_RETURN_INT64(val1 / val2);
}

Datum
int48pl(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(val1 + val2);
}

Datum
int48mi(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(val1 - val2);
}

Datum
int48mul(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(val1 * val2);
}

Datum
int48div(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	if (val2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	PG_RETURN_INT64(val1 / val2);
}

/* Binary arithmetics
 *
 *		int8and		- returns arg1 & arg2
 *		int8or		- returns arg1 | arg2
 *		int8xor		- returns arg1 # arg2
 *		int8not		- returns ~arg1
 *		int8shl		- returns arg1 << arg2
 *		int8shr		- returns arg1 >> arg2
 */

Datum
int8and(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(arg1 & arg2);
}

Datum
int8or(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(arg1 | arg2);
}

Datum
int8xor(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(arg1 ^ arg2);
}

Datum
int8not(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);

	PG_RETURN_INT64(~arg1);
}

Datum
int8shl(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT64(arg1 << arg2);
}

Datum
int8shr(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT64(arg1 >> arg2);
}

/*----------------------------------------------------------
 *	Conversion operators.
 *---------------------------------------------------------*/

Datum
int48(PG_FUNCTION_ARGS)
{
	int32		val = PG_GETARG_INT32(0);

	PG_RETURN_INT64((int64) val);
}

Datum
int84(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	int32		result;

	result = (int32) val;

	/* Test for overflow by reverse-conversion. */
	if ((int64) result != val)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32(result);
}

Datum
int28(PG_FUNCTION_ARGS)
{
	int16		val = PG_GETARG_INT16(0);

	PG_RETURN_INT64((int64) val);
}

Datum
int82(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	int16		result;

	result = (int16) val;

	/* Test for overflow by reverse-conversion. */
	if ((int64) result != val)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT16(result);
}

Datum
i8tod(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	float8		result;

	result = val;

	PG_RETURN_FLOAT8(result);
}

/* dtoi8()
 * Convert float8 to 8-byte integer.
 */
Datum
dtoi8(PG_FUNCTION_ARGS)
{
	float8		val = PG_GETARG_FLOAT8(0);
	int64		result;

	/* Round val to nearest integer (but it's still in float form) */
	val = rint(val);

	/*
	 * Does it fit in an int64?  Avoid assuming that we have handy
	 * constants defined for the range boundaries, instead test for
	 * overflow by reverse-conversion.
	 */
	result = (int64) val;

	if ((float8) result != val)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT64(result);
}

Datum
i8tof(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	float4		result;

	result = val;

	PG_RETURN_FLOAT4(result);
}

/* ftoi8()
 * Convert float4 to 8-byte integer.
 */
Datum
ftoi8(PG_FUNCTION_ARGS)
{
	float4		val = PG_GETARG_FLOAT4(0);
	int64		result;
	float8		dval;

	/* Round val to nearest integer (but it's still in float form) */
	dval = rint(val);

	/*
	 * Does it fit in an int64?  Avoid assuming that we have handy
	 * constants defined for the range boundaries, instead test for
	 * overflow by reverse-conversion.
	 */
	result = (int64) dval;

	if ((float8) result != dval)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT64(result);
}

Datum
i8tooid(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	Oid			result;

	result = (Oid) val;

	/* Test for overflow by reverse-conversion. */
	if ((int64) result != val)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("OID out of range")));

	PG_RETURN_OID(result);
}

Datum
oidtoi8(PG_FUNCTION_ARGS)
{
	Oid			val = PG_GETARG_OID(0);

	PG_RETURN_INT64((int64) val);
}

Datum
text_int8(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	int			len;
	char	   *s;
	Datum		result;

	len = (VARSIZE(str) - VARHDRSZ);
	s = palloc(len + 1);
	memcpy(s, VARDATA(str), len);
	*(s + len) = '\0';

	result = DirectFunctionCall1(int8in, CStringGetDatum(s));

	pfree(s);

	return result;
}

Datum
int8_text(PG_FUNCTION_ARGS)
{
	/* val is int64, but easier to leave it as Datum */
	Datum		val = PG_GETARG_DATUM(0);
	char	   *s;
	int			len;
	text	   *result;

	s = DatumGetCString(DirectFunctionCall1(int8out, val));
	len = strlen(s);

	result = (text *) palloc(VARHDRSZ + len);

	VARATT_SIZEP(result) = len + VARHDRSZ;
	memcpy(VARDATA(result), s, len);

	pfree(s);

	PG_RETURN_TEXT_P(result);
}
