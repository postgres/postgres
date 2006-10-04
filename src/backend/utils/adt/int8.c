/*-------------------------------------------------------------------------
 *
 * int8.c
 *	  Internal 64-bit integer operations
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/int8.c,v 1.62 2006/10/04 00:29:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>

#include "funcapi.h"
#include "libpq/pqformat.h"
#include "nodes/nodes.h"
#include "utils/int8.h"


#define MAXINT8LEN		25

#define SAMESIGN(a,b)	(((a) < 0) == ((b) < 0))

typedef struct
{
	int64		current;
	int64		finish;
	int64		step;
} generate_series_fctx;


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
	 * Do our own scan, rather than relying on sscanf which might be broken
	 * for long long.
	 */

	/* skip leading spaces */
	while (*ptr && isspace((unsigned char) *ptr))
		ptr++;

	/* handle sign */
	if (*ptr == '-')
	{
		ptr++;

		/*
		 * Do an explicit check for INT64_MIN.	Ugly though this is, it's
		 * cleaner than trying to get the loop below to handle it portably.
		 */
#ifndef INT64_IS_BUSTED
		if (strncmp(ptr, "9223372036854775808", 19) == 0)
		{
			tmp = -INT64CONST(0x7fffffffffffffff) - 1;
			ptr += 19;
			goto gotdigits;
		}
#endif

		sign = -1;
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
					 errmsg("invalid input syntax for integer: \"%s\"",
							str)));
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
					   errmsg("value \"%s\" is out of range for type bigint",
							  str)));
		}
		tmp = newtmp;
	}

gotdigits:

	/* allow trailing whitespace, but not other trailing chars */
	while (*ptr != '\0' && isspace((unsigned char) *ptr))
		ptr++;

	if (*ptr != '\0')
	{
		if (errorOK)
			return false;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for integer: \"%s\"",
							str)));
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
	int64		arg = PG_GETARG_INT64(0);
	int64		result;

	result = -arg;
	/* overflow check (needed for INT64_MIN) */
	if (arg != 0 && SAMESIGN(result, arg))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int8up(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);

	PG_RETURN_INT64(arg);
}

Datum
int8pl(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = arg1 + arg2;

	/*
	 * Overflow check.	If the inputs are of different signs then their sum
	 * cannot overflow.  If the inputs are of the same sign, their sum had
	 * better be that sign too.
	 */
	if (SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int8mi(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = arg1 - arg2;

	/*
	 * Overflow check.	If the inputs are of the same sign then their
	 * difference cannot overflow.	If they are of different signs then the
	 * result should be of the same sign as the first input.
	 */
	if (!SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int8mul(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = arg1 * arg2;

	/*
	 * Overflow check.	We basically check to see if result / arg2 gives arg1
	 * again.  There are two cases where this fails: arg2 = 0 (which cannot
	 * overflow) and arg1 = INT64_MIN, arg2 = -1 (where the division itself
	 * will overflow and thus incorrectly match).
	 *
	 * Since the division is likely much more expensive than the actual
	 * multiplication, we'd like to skip it where possible.  The best bang for
	 * the buck seems to be to check whether both inputs are in the int32
	 * range; if so, no overflow is possible.
	 */
	if (!(arg1 == (int64) ((int32) arg1) &&
		  arg2 == (int64) ((int32) arg2)) &&
		arg2 != 0 &&
		(result / arg2 != arg1 || (arg2 == -1 && arg1 < 0 && result < 0)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int8div(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (arg2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = arg1 / arg2;

	/*
	 * Overflow check.	The only possible overflow case is for arg1 =
	 * INT64_MIN, arg2 = -1, where the correct result is -INT64_MIN, which
	 * can't be represented on a two's-complement machine.
	 */
	if (arg2 == -1 && arg1 < 0 && result < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

/* int8abs()
 * Absolute value
 */
Datum
int8abs(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		result;

	result = (arg1 < 0) ? -arg1 : arg1;
	/* overflow check (needed for INT64_MIN) */
	if (result < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

/* int8mod()
 * Modulo operation.
 */
Datum
int8mod(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);

	if (arg2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
	/* No overflow is possible */

	PG_RETURN_INT64(arg1 % arg2);
}


Datum
int8inc(PG_FUNCTION_ARGS)
{
	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		/*
		 * Special case to avoid palloc overhead for COUNT(): when called from
		 * nodeAgg, we know that the argument is modifiable local storage, so
		 * just update it in-place.
		 *
		 * Note: this assumes int8 is a pass-by-ref type; if we ever support
		 * pass-by-val int8, this should be ifdef'd out when int8 is
		 * pass-by-val.
		 */
		int64	   *arg = (int64 *) PG_GETARG_POINTER(0);
		int64		result;

		result = *arg + 1;
		/* Overflow check */
		if (result < 0 && *arg > 0)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));

		*arg = result;
		PG_RETURN_POINTER(arg);
	}
	else
	{
		/* Not called by nodeAgg, so just do it the dumb way */
		int64		arg = PG_GETARG_INT64(0);
		int64		result;

		result = arg + 1;
		/* Overflow check */
		if (result < 0 && arg > 0)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));

		PG_RETURN_INT64(result);
	}
}

/*
 * These functions are exactly like int8inc but are used for aggregates that
 * count only non-null values.	Since the functions are declared strict,
 * the null checks happen before we ever get here, and all we need do is
 * increment the state value.  We could actually make these pg_proc entries
 * point right at int8inc, but then the opr_sanity regression test would
 * complain about mismatched entries for a built-in function.
 */

Datum
int8inc_any(PG_FUNCTION_ARGS)
{
	return int8inc(fcinfo);
}

Datum
int8inc_float8_float8(PG_FUNCTION_ARGS)
{
	return int8inc(fcinfo);
}


Datum
int8larger(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = ((arg1 > arg2) ? arg1 : arg2);

	PG_RETURN_INT64(result);
}

Datum
int8smaller(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = ((arg1 < arg2) ? arg1 : arg2);

	PG_RETURN_INT64(result);
}

Datum
int84pl(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int64		result;

	result = arg1 + arg2;

	/*
	 * Overflow check.	If the inputs are of different signs then their sum
	 * cannot overflow.  If the inputs are of the same sign, their sum had
	 * better be that sign too.
	 */
	if (SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int84mi(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int64		result;

	result = arg1 - arg2;

	/*
	 * Overflow check.	If the inputs are of the same sign then their
	 * difference cannot overflow.	If they are of different signs then the
	 * result should be of the same sign as the first input.
	 */
	if (!SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int84mul(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int64		result;

	result = arg1 * arg2;

	/*
	 * Overflow check.	We basically check to see if result / arg1 gives arg2
	 * again.  There is one case where this fails: arg1 = 0 (which cannot
	 * overflow).
	 *
	 * Since the division is likely much more expensive than the actual
	 * multiplication, we'd like to skip it where possible.  The best bang for
	 * the buck seems to be to check whether both inputs are in the int32
	 * range; if so, no overflow is possible.
	 */
	if (arg1 != (int64) ((int32) arg1) &&
		result / arg1 != arg2)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int84div(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int64		result;

	if (arg2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = arg1 / arg2;

	/*
	 * Overflow check.	The only possible overflow case is for arg1 =
	 * INT64_MIN, arg2 = -1, where the correct result is -INT64_MIN, which
	 * can't be represented on a two's-complement machine.
	 */
	if (arg2 == -1 && arg1 < 0 && result < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int48pl(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = arg1 + arg2;

	/*
	 * Overflow check.	If the inputs are of different signs then their sum
	 * cannot overflow.  If the inputs are of the same sign, their sum had
	 * better be that sign too.
	 */
	if (SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int48mi(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = arg1 - arg2;

	/*
	 * Overflow check.	If the inputs are of the same sign then their
	 * difference cannot overflow.	If they are of different signs then the
	 * result should be of the same sign as the first input.
	 */
	if (!SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int48mul(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = arg1 * arg2;

	/*
	 * Overflow check.	We basically check to see if result / arg2 gives arg1
	 * again.  There is one case where this fails: arg2 = 0 (which cannot
	 * overflow).
	 *
	 * Since the division is likely much more expensive than the actual
	 * multiplication, we'd like to skip it where possible.  The best bang for
	 * the buck seems to be to check whether both inputs are in the int32
	 * range; if so, no overflow is possible.
	 */
	if (arg2 != (int64) ((int32) arg2) &&
		result / arg2 != arg1)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int48div(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int64		arg2 = PG_GETARG_INT64(1);

	if (arg2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
	/* No overflow is possible */
	PG_RETURN_INT64((int64) arg1 / arg2);
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
	int32		arg = PG_GETARG_INT32(0);

	PG_RETURN_INT64((int64) arg);
}

Datum
int84(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);
	int32		result;

	result = (int32) arg;

	/* Test for overflow by reverse-conversion. */
	if ((int64) result != arg)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32(result);
}

Datum
int28(PG_FUNCTION_ARGS)
{
	int16		arg = PG_GETARG_INT16(0);

	PG_RETURN_INT64((int64) arg);
}

Datum
int82(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);
	int16		result;

	result = (int16) arg;

	/* Test for overflow by reverse-conversion. */
	if ((int64) result != arg)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16(result);
}

Datum
i8tod(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);
	float8		result;

	result = arg;

	PG_RETURN_FLOAT8(result);
}

/* dtoi8()
 * Convert float8 to 8-byte integer.
 */
Datum
dtoi8(PG_FUNCTION_ARGS)
{
	float8		arg = PG_GETARG_FLOAT8(0);
	int64		result;

	/* Round arg to nearest integer (but it's still in float form) */
	arg = rint(arg);

	/*
	 * Does it fit in an int64?  Avoid assuming that we have handy constants
	 * defined for the range boundaries, instead test for overflow by
	 * reverse-conversion.
	 */
	result = (int64) arg;

	if ((float8) result != arg)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	PG_RETURN_INT64(result);
}

Datum
i8tof(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);
	float4		result;

	result = arg;

	PG_RETURN_FLOAT4(result);
}

/* ftoi8()
 * Convert float4 to 8-byte integer.
 */
Datum
ftoi8(PG_FUNCTION_ARGS)
{
	float4		arg = PG_GETARG_FLOAT4(0);
	int64		result;
	float8		darg;

	/* Round arg to nearest integer (but it's still in float form) */
	darg = rint(arg);

	/*
	 * Does it fit in an int64?  Avoid assuming that we have handy constants
	 * defined for the range boundaries, instead test for overflow by
	 * reverse-conversion.
	 */
	result = (int64) darg;

	if ((float8) result != darg)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	PG_RETURN_INT64(result);
}

Datum
i8tooid(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);
	Oid			result;

	result = (Oid) arg;

	/* Test for overflow by reverse-conversion. */
	if ((int64) result != arg)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("OID out of range")));

	PG_RETURN_OID(result);
}

Datum
oidtoi8(PG_FUNCTION_ARGS)
{
	Oid			arg = PG_GETARG_OID(0);

	PG_RETURN_INT64((int64) arg);
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
	/* arg is int64, but easier to leave it as Datum */
	Datum		arg = PG_GETARG_DATUM(0);
	char	   *s;
	int			len;
	text	   *result;

	s = DatumGetCString(DirectFunctionCall1(int8out, arg));
	len = strlen(s);

	result = (text *) palloc(VARHDRSZ + len);

	VARATT_SIZEP(result) = len + VARHDRSZ;
	memcpy(VARDATA(result), s, len);

	pfree(s);

	PG_RETURN_TEXT_P(result);
}

/*
 * non-persistent numeric series generator
 */
Datum
generate_series_int8(PG_FUNCTION_ARGS)
{
	return generate_series_step_int8(fcinfo);
}

Datum
generate_series_step_int8(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	generate_series_fctx *fctx;
	int64		result;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		int64		start = PG_GETARG_INT64(0);
		int64		finish = PG_GETARG_INT64(1);
		int64		step = 1;

		/* see if we were given an explicit step size */
		if (PG_NARGS() == 3)
			step = PG_GETARG_INT64(2);
		if (step == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size may not equal zero")));

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* allocate memory for user context */
		fctx = (generate_series_fctx *) palloc(sizeof(generate_series_fctx));

		/*
		 * Use fctx to keep state from call to call. Seed current with the
		 * original start value
		 */
		fctx->current = start;
		fctx->finish = finish;
		fctx->step = step;

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * get the saved state and use current as the result for this iteration
	 */
	fctx = funcctx->user_fctx;
	result = fctx->current;

	if ((fctx->step > 0 && fctx->current <= fctx->finish) ||
		(fctx->step < 0 && fctx->current >= fctx->finish))
	{
		/* increment current in preparation for next iteration */
		fctx->current += fctx->step;

		/* do when there is more left to send */
		SRF_RETURN_NEXT(funcctx, Int64GetDatum(result));
	}
	else
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
}
