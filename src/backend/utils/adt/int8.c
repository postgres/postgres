/*-------------------------------------------------------------------------
 *
 * int8.c
 *	  Internal 64-bit integer operations
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/int8.c,v 1.26 2000/12/03 20:45:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <time.h>
#include <math.h>
#include <float.h>
#include <limits.h>

#include "utils/int8.h"

/* this should be set in config.h, but just in case it wasn't: */
#ifndef INT64_FORMAT
#define INT64_FORMAT "%ld"
#endif

#define MAXINT8LEN		25

#ifndef INT_MAX
#define INT_MAX (0x7FFFFFFFL)
#endif
#ifndef INT_MIN
#define INT_MIN (-INT_MAX-1)
#endif
#ifndef SHRT_MAX
#define SHRT_MAX (0x7FFF)
#endif
#ifndef SHRT_MIN
#define SHRT_MIN (-SHRT_MAX-1)
#endif


/***********************************************************************
 **
 **		Routines for 64-bit integers.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * Formatting and conversion routines.
 *---------------------------------------------------------*/

/* int8in()
 */
Datum
int8in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	int64		result;
	char	   *ptr = str;
	int64		tmp = 0;
	int			sign = 1;

	/*
	 * Do our own scan, rather than relying on sscanf which might be
	 * broken for long long.
	 */
	while (*ptr && isspace((unsigned char) *ptr))	/* skip leading spaces */
		ptr++;
	if (*ptr == '-')			/* handle sign */
		sign = -1, ptr++;
	else if (*ptr == '+')
		ptr++;
	if (!isdigit((unsigned char) *ptr))		/* require at least one digit */
		elog(ERROR, "Bad int8 external representation \"%s\"", str);
	while (*ptr && isdigit((unsigned char) *ptr))	/* process digits */
	{
		int64		newtmp = tmp * 10 + (*ptr++ - '0');

		if ((newtmp / 10) != tmp)		/* overflow? */
			elog(ERROR, "int8 value out of range: \"%s\"", str);
		tmp = newtmp;
	}
	if (*ptr)					/* trailing junk? */
		elog(ERROR, "Bad int8 external representation \"%s\"", str);

	result = (sign < 0) ? -tmp : tmp;

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
		elog(ERROR, "Unable to format int8");

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
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

	PG_RETURN_INT64(- val);
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

	if (arg1 < 1)
		result = 0;
	else
		for (i = arg1, result = 1; i > 0; --i)
			result *= i;

	PG_RETURN_INT64(result);
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

	if ((val < INT_MIN) || (val > INT_MAX))
		elog(ERROR, "int8 conversion to int4 is out of range");

	result = (int32) val;

	PG_RETURN_INT32(result);
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
 * Convert double float to 8-byte integer.
 * Do a range check before the conversion.
 * Note that the comparison probably isn't quite right
 *	since we only have ~52 bits of precision in a double float
 *	and so subtracting one from a large number gives the large
 *	number exactly. However, for some reason the comparison below
 *	does the right thing on my i686/linux-rh4.2 box.
 * - thomas 1998-06-16
 */
Datum
dtoi8(PG_FUNCTION_ARGS)
{
	float8		val = PG_GETARG_FLOAT8(0);
	int64		result;

	if ((val < (-pow(2.0, 63.0) + 1)) || (val > (pow(2.0, 63.0) - 1)))
		elog(ERROR, "Floating point conversion to int64 is out of range");

	result = (int64) val;

	PG_RETURN_INT64(result);
}

/* text_int8()
 */
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


/* int8_text()
 */
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
