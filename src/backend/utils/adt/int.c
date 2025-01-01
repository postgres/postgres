/*-------------------------------------------------------------------------
 *
 * int.c
 *	  Functions for the built-in integer types (except int8).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/int.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * OLD COMMENTS
 *		I/O routines:
 *		 int2in, int2out, int2recv, int2send
 *		 int4in, int4out, int4recv, int4send
 *		 int2vectorin, int2vectorout, int2vectorrecv, int2vectorsend
 *		Boolean operators:
 *		 inteq, intne, intlt, intle, intgt, intge
 *		Arithmetic operators:
 *		 intpl, intmi, int4mul, intdiv
 *
 *		Arithmetic operators:
 *		 intmod
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>

#include "catalog/pg_type.h"
#include "common/int.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/builtins.h"

#define Int2VectorSize(n)	(offsetof(int2vector, values) + (n) * sizeof(int16))

typedef struct
{
	int32		current;
	int32		finish;
	int32		step;
} generate_series_fctx;


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		int2in			- converts "num" to short
 */
Datum
int2in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);

	PG_RETURN_INT16(pg_strtoint16_safe(num, fcinfo->context));
}

/*
 *		int2out			- converts short to "num"
 */
Datum
int2out(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	char	   *result = (char *) palloc(7);	/* sign, 5 digits, '\0' */

	pg_itoa(arg1, result);
	PG_RETURN_CSTRING(result);
}

/*
 *		int2recv			- converts external binary format to int2
 */
Datum
int2recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INT16((int16) pq_getmsgint(buf, sizeof(int16)));
}

/*
 *		int2send			- converts int2 to binary format
 */
Datum
int2send(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint16(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * construct int2vector given a raw array of int2s
 *
 * If int2s is NULL then caller must fill values[] afterward
 */
int2vector *
buildint2vector(const int16 *int2s, int n)
{
	int2vector *result;

	result = (int2vector *) palloc0(Int2VectorSize(n));

	if (n > 0 && int2s)
		memcpy(result->values, int2s, n * sizeof(int16));

	/*
	 * Attach standard array header.  For historical reasons, we set the index
	 * lower bound to 0 not 1.
	 */
	SET_VARSIZE(result, Int2VectorSize(n));
	result->ndim = 1;
	result->dataoffset = 0;		/* never any nulls */
	result->elemtype = INT2OID;
	result->dim1 = n;
	result->lbound1 = 0;

	return result;
}

/*
 *		int2vectorin			- converts "num num ..." to internal form
 */
Datum
int2vectorin(PG_FUNCTION_ARGS)
{
	char	   *intString = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	int2vector *result;
	int			nalloc;
	int			n;

	nalloc = 32;				/* arbitrary initial size guess */
	result = (int2vector *) palloc0(Int2VectorSize(nalloc));

	for (n = 0;; n++)
	{
		long		l;
		char	   *endp;

		while (*intString && isspace((unsigned char) *intString))
			intString++;
		if (*intString == '\0')
			break;

		if (n >= nalloc)
		{
			nalloc *= 2;
			result = (int2vector *) repalloc(result, Int2VectorSize(nalloc));
		}

		errno = 0;
		l = strtol(intString, &endp, 10);

		if (intString == endp)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							"smallint", intString)));

		if (errno == ERANGE || l < SHRT_MIN || l > SHRT_MAX)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value \"%s\" is out of range for type %s", intString,
							"smallint")));

		if (*endp && *endp != ' ')
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							"smallint", intString)));

		result->values[n] = l;
		intString = endp;
	}

	SET_VARSIZE(result, Int2VectorSize(n));
	result->ndim = 1;
	result->dataoffset = 0;		/* never any nulls */
	result->elemtype = INT2OID;
	result->dim1 = n;
	result->lbound1 = 0;

	PG_RETURN_POINTER(result);
}

/*
 *		int2vectorout		- converts internal form to "num num ..."
 */
Datum
int2vectorout(PG_FUNCTION_ARGS)
{
	int2vector *int2Array = (int2vector *) PG_GETARG_POINTER(0);
	int			num,
				nnums = int2Array->dim1;
	char	   *rp;
	char	   *result;

	/* assumes sign, 5 digits, ' ' */
	rp = result = (char *) palloc(nnums * 7 + 1);
	for (num = 0; num < nnums; num++)
	{
		if (num != 0)
			*rp++ = ' ';
		rp += pg_itoa(int2Array->values[num], rp);
	}
	*rp = '\0';
	PG_RETURN_CSTRING(result);
}

/*
 *		int2vectorrecv			- converts external binary format to int2vector
 */
Datum
int2vectorrecv(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(locfcinfo, 3);
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int2vector *result;

	/*
	 * Normally one would call array_recv() using DirectFunctionCall3, but
	 * that does not work since array_recv wants to cache some data using
	 * fcinfo->flinfo->fn_extra.  So we need to pass it our own flinfo
	 * parameter.
	 */
	InitFunctionCallInfoData(*locfcinfo, fcinfo->flinfo, 3,
							 InvalidOid, NULL, NULL);

	locfcinfo->args[0].value = PointerGetDatum(buf);
	locfcinfo->args[0].isnull = false;
	locfcinfo->args[1].value = ObjectIdGetDatum(INT2OID);
	locfcinfo->args[1].isnull = false;
	locfcinfo->args[2].value = Int32GetDatum(-1);
	locfcinfo->args[2].isnull = false;

	result = (int2vector *) DatumGetPointer(array_recv(locfcinfo));

	Assert(!locfcinfo->isnull);

	/* sanity checks: int2vector must be 1-D, 0-based, no nulls */
	if (ARR_NDIM(result) != 1 ||
		ARR_HASNULL(result) ||
		ARR_ELEMTYPE(result) != INT2OID ||
		ARR_LBOUND(result)[0] != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid int2vector data")));

	PG_RETURN_POINTER(result);
}

/*
 *		int2vectorsend			- converts int2vector to binary format
 */
Datum
int2vectorsend(PG_FUNCTION_ARGS)
{
	return array_send(fcinfo);
}


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

/*
 *		int4in			- converts "num" to int4
 */
Datum
int4in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);

	PG_RETURN_INT32(pg_strtoint32_safe(num, fcinfo->context));
}

/*
 *		int4out			- converts int4 to "num"
 */
Datum
int4out(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	char	   *result = (char *) palloc(12);	/* sign, 10 digits, '\0' */

	pg_ltoa(arg1, result);
	PG_RETURN_CSTRING(result);
}

/*
 *		int4recv			- converts external binary format to int4
 */
Datum
int4recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INT32((int32) pq_getmsgint(buf, sizeof(int32)));
}

/*
 *		int4send			- converts int4 to binary format
 */
Datum
int4send(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 *		===================
 *		CONVERSION ROUTINES
 *		===================
 */

Datum
i2toi4(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);

	PG_RETURN_INT32((int32) arg1);
}

Datum
i4toi2(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);

	if (unlikely(arg1 < SHRT_MIN) || unlikely(arg1 > SHRT_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) arg1);
}

/* Cast int4 -> bool */
Datum
int4_bool(PG_FUNCTION_ARGS)
{
	if (PG_GETARG_INT32(0) == 0)
		PG_RETURN_BOOL(false);
	else
		PG_RETURN_BOOL(true);
}

/* Cast bool -> int4 */
Datum
bool_int4(PG_FUNCTION_ARGS)
{
	if (PG_GETARG_BOOL(0) == false)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(1);
}

/*
 *		============================
 *		COMPARISON OPERATOR ROUTINES
 *		============================
 */

/*
 *		inteq			- returns 1 iff arg1 == arg2
 *		intne			- returns 1 iff arg1 != arg2
 *		intlt			- returns 1 iff arg1 < arg2
 *		intle			- returns 1 iff arg1 <= arg2
 *		intgt			- returns 1 iff arg1 > arg2
 *		intge			- returns 1 iff arg1 >= arg2
 */

Datum
int4eq(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
int4ne(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
int4lt(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
int4le(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
int4gt(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
int4ge(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}

Datum
int2eq(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
int2ne(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
int2lt(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
int2le(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
int2gt(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
int2ge(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}

Datum
int24eq(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
int24ne(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
int24lt(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
int24le(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
int24gt(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
int24ge(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}

Datum
int42eq(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
int42ne(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
int42lt(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
int42le(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
int42gt(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
int42ge(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}


/*----------------------------------------------------------
 *	in_range functions for int4 and int2,
 *	including cross-data-type comparisons.
 *
 *	Note: we provide separate intN_int8 functions for performance
 *	reasons.  This forces also providing intN_int2, else cases with a
 *	smallint offset value would fail to resolve which function to use.
 *	But that's an unlikely situation, so don't duplicate code for it.
 *---------------------------------------------------------*/

Datum
in_range_int4_int4(PG_FUNCTION_ARGS)
{
	int32		val = PG_GETARG_INT32(0);
	int32		base = PG_GETARG_INT32(1);
	int32		offset = PG_GETARG_INT32(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	int32		sum;

	if (offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	if (sub)
		offset = -offset;		/* cannot overflow */

	if (unlikely(pg_add_s32_overflow(base, offset, &sum)))
	{
		/*
		 * If sub is false, the true sum is surely more than val, so correct
		 * answer is the same as "less".  If sub is true, the true sum is
		 * surely less than val, so the answer is "!less".
		 */
		PG_RETURN_BOOL(sub ? !less : less);
	}

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

Datum
in_range_int4_int2(PG_FUNCTION_ARGS)
{
	/* Doesn't seem worth duplicating code for, so just invoke int4_int4 */
	return DirectFunctionCall5(in_range_int4_int4,
							   PG_GETARG_DATUM(0),
							   PG_GETARG_DATUM(1),
							   Int32GetDatum((int32) PG_GETARG_INT16(2)),
							   PG_GETARG_DATUM(3),
							   PG_GETARG_DATUM(4));
}

Datum
in_range_int4_int8(PG_FUNCTION_ARGS)
{
	/* We must do all the math in int64 */
	int64		val = (int64) PG_GETARG_INT32(0);
	int64		base = (int64) PG_GETARG_INT32(1);
	int64		offset = PG_GETARG_INT64(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	int64		sum;

	if (offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	if (sub)
		offset = -offset;		/* cannot overflow */

	if (unlikely(pg_add_s64_overflow(base, offset, &sum)))
	{
		/*
		 * If sub is false, the true sum is surely more than val, so correct
		 * answer is the same as "less".  If sub is true, the true sum is
		 * surely less than val, so the answer is "!less".
		 */
		PG_RETURN_BOOL(sub ? !less : less);
	}

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

Datum
in_range_int2_int4(PG_FUNCTION_ARGS)
{
	/* We must do all the math in int32 */
	int32		val = (int32) PG_GETARG_INT16(0);
	int32		base = (int32) PG_GETARG_INT16(1);
	int32		offset = PG_GETARG_INT32(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	int32		sum;

	if (offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	if (sub)
		offset = -offset;		/* cannot overflow */

	if (unlikely(pg_add_s32_overflow(base, offset, &sum)))
	{
		/*
		 * If sub is false, the true sum is surely more than val, so correct
		 * answer is the same as "less".  If sub is true, the true sum is
		 * surely less than val, so the answer is "!less".
		 */
		PG_RETURN_BOOL(sub ? !less : less);
	}

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

Datum
in_range_int2_int2(PG_FUNCTION_ARGS)
{
	/* Doesn't seem worth duplicating code for, so just invoke int2_int4 */
	return DirectFunctionCall5(in_range_int2_int4,
							   PG_GETARG_DATUM(0),
							   PG_GETARG_DATUM(1),
							   Int32GetDatum((int32) PG_GETARG_INT16(2)),
							   PG_GETARG_DATUM(3),
							   PG_GETARG_DATUM(4));
}

Datum
in_range_int2_int8(PG_FUNCTION_ARGS)
{
	/* Doesn't seem worth duplicating code for, so just invoke int4_int8 */
	return DirectFunctionCall5(in_range_int4_int8,
							   Int32GetDatum((int32) PG_GETARG_INT16(0)),
							   Int32GetDatum((int32) PG_GETARG_INT16(1)),
							   PG_GETARG_DATUM(2),
							   PG_GETARG_DATUM(3),
							   PG_GETARG_DATUM(4));
}


/*
 *		int[24]pl		- returns arg1 + arg2
 *		int[24]mi		- returns arg1 - arg2
 *		int[24]mul		- returns arg1 * arg2
 *		int[24]div		- returns arg1 / arg2
 */

Datum
int4um(PG_FUNCTION_ARGS)
{
	int32		arg = PG_GETARG_INT32(0);

	if (unlikely(arg == PG_INT32_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(-arg);
}

Datum
int4up(PG_FUNCTION_ARGS)
{
	int32		arg = PG_GETARG_INT32(0);

	PG_RETURN_INT32(arg);
}

Datum
int4pl(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (unlikely(pg_add_s32_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int4mi(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (unlikely(pg_sub_s32_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int4mul(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (unlikely(pg_mul_s32_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int4div(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (arg2 == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* ensure compiler realizes we mustn't reach the division (gcc bug) */
		PG_RETURN_NULL();
	}

	/*
	 * INT_MIN / -1 is problematic, since the result can't be represented on a
	 * two's-complement machine.  Some machines produce INT_MIN, some produce
	 * zero, some throw an exception.  We can dodge the problem by recognizing
	 * that division by -1 is the same as negation.
	 */
	if (arg2 == -1)
	{
		if (unlikely(arg1 == PG_INT32_MIN))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
		result = -arg1;
		PG_RETURN_INT32(result);
	}

	/* No overflow is possible */

	result = arg1 / arg2;

	PG_RETURN_INT32(result);
}

Datum
int4inc(PG_FUNCTION_ARGS)
{
	int32		arg = PG_GETARG_INT32(0);
	int32		result;

	if (unlikely(pg_add_s32_overflow(arg, 1, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32(result);
}

Datum
int2um(PG_FUNCTION_ARGS)
{
	int16		arg = PG_GETARG_INT16(0);

	if (unlikely(arg == PG_INT16_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));
	PG_RETURN_INT16(-arg);
}

Datum
int2up(PG_FUNCTION_ARGS)
{
	int16		arg = PG_GETARG_INT16(0);

	PG_RETURN_INT16(arg);
}

Datum
int2pl(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int16		result;

	if (unlikely(pg_add_s16_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));
	PG_RETURN_INT16(result);
}

Datum
int2mi(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int16		result;

	if (unlikely(pg_sub_s16_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));
	PG_RETURN_INT16(result);
}

Datum
int2mul(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int16		result;

	if (unlikely(pg_mul_s16_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16(result);
}

Datum
int2div(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int16		result;

	if (arg2 == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* ensure compiler realizes we mustn't reach the division (gcc bug) */
		PG_RETURN_NULL();
	}

	/*
	 * SHRT_MIN / -1 is problematic, since the result can't be represented on
	 * a two's-complement machine.  Some machines produce SHRT_MIN, some
	 * produce zero, some throw an exception.  We can dodge the problem by
	 * recognizing that division by -1 is the same as negation.
	 */
	if (arg2 == -1)
	{
		if (unlikely(arg1 == PG_INT16_MIN))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("smallint out of range")));
		result = -arg1;
		PG_RETURN_INT16(result);
	}

	/* No overflow is possible */

	result = arg1 / arg2;

	PG_RETURN_INT16(result);
}

Datum
int24pl(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (unlikely(pg_add_s32_overflow((int32) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int24mi(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (unlikely(pg_sub_s32_overflow((int32) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int24mul(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (unlikely(pg_mul_s32_overflow((int32) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int24div(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* ensure compiler realizes we mustn't reach the division (gcc bug) */
		PG_RETURN_NULL();
	}

	/* No overflow is possible */
	PG_RETURN_INT32((int32) arg1 / arg2);
}

Datum
int42pl(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int32		result;

	if (unlikely(pg_add_s32_overflow(arg1, (int32) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int42mi(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int32		result;

	if (unlikely(pg_sub_s32_overflow(arg1, (int32) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int42mul(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int32		result;

	if (unlikely(pg_mul_s32_overflow(arg1, (int32) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int42div(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int32		result;

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* ensure compiler realizes we mustn't reach the division (gcc bug) */
		PG_RETURN_NULL();
	}

	/*
	 * INT_MIN / -1 is problematic, since the result can't be represented on a
	 * two's-complement machine.  Some machines produce INT_MIN, some produce
	 * zero, some throw an exception.  We can dodge the problem by recognizing
	 * that division by -1 is the same as negation.
	 */
	if (arg2 == -1)
	{
		if (unlikely(arg1 == PG_INT32_MIN))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
		result = -arg1;
		PG_RETURN_INT32(result);
	}

	/* No overflow is possible */

	result = arg1 / arg2;

	PG_RETURN_INT32(result);
}

Datum
int4mod(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* ensure compiler realizes we mustn't reach the division (gcc bug) */
		PG_RETURN_NULL();
	}

	/*
	 * Some machines throw a floating-point exception for INT_MIN % -1, which
	 * is a bit silly since the correct answer is perfectly well-defined,
	 * namely zero.
	 */
	if (arg2 == -1)
		PG_RETURN_INT32(0);

	/* No overflow is possible */

	PG_RETURN_INT32(arg1 % arg2);
}

Datum
int2mod(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* ensure compiler realizes we mustn't reach the division (gcc bug) */
		PG_RETURN_NULL();
	}

	/*
	 * Some machines throw a floating-point exception for INT_MIN % -1, which
	 * is a bit silly since the correct answer is perfectly well-defined,
	 * namely zero.  (It's not clear this ever happens when dealing with
	 * int16, but we might as well have the test for safety.)
	 */
	if (arg2 == -1)
		PG_RETURN_INT16(0);

	/* No overflow is possible */

	PG_RETURN_INT16(arg1 % arg2);
}


/* int[24]abs()
 * Absolute value
 */
Datum
int4abs(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		result;

	if (unlikely(arg1 == PG_INT32_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	result = (arg1 < 0) ? -arg1 : arg1;
	PG_RETURN_INT32(result);
}

Datum
int2abs(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		result;

	if (unlikely(arg1 == PG_INT16_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));
	result = (arg1 < 0) ? -arg1 : arg1;
	PG_RETURN_INT16(result);
}

/*
 * Greatest Common Divisor
 *
 * Returns the largest positive integer that exactly divides both inputs.
 * Special cases:
 *   - gcd(x, 0) = gcd(0, x) = abs(x)
 *   		because 0 is divisible by anything
 *   - gcd(0, 0) = 0
 *   		complies with the previous definition and is a common convention
 *
 * Special care must be taken if either input is INT_MIN --- gcd(0, INT_MIN),
 * gcd(INT_MIN, 0) and gcd(INT_MIN, INT_MIN) are all equal to abs(INT_MIN),
 * which cannot be represented as a 32-bit signed integer.
 */
static int32
int4gcd_internal(int32 arg1, int32 arg2)
{
	int32		swap;
	int32		a1,
				a2;

	/*
	 * Put the greater absolute value in arg1.
	 *
	 * This would happen automatically in the loop below, but avoids an
	 * expensive modulo operation, and simplifies the special-case handling
	 * for INT_MIN below.
	 *
	 * We do this in negative space in order to handle INT_MIN.
	 */
	a1 = (arg1 < 0) ? arg1 : -arg1;
	a2 = (arg2 < 0) ? arg2 : -arg2;
	if (a1 > a2)
	{
		swap = arg1;
		arg1 = arg2;
		arg2 = swap;
	}

	/* Special care needs to be taken with INT_MIN.  See comments above. */
	if (arg1 == PG_INT32_MIN)
	{
		if (arg2 == 0 || arg2 == PG_INT32_MIN)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));

		/*
		 * Some machines throw a floating-point exception for INT_MIN % -1,
		 * which is a bit silly since the correct answer is perfectly
		 * well-defined, namely zero.  Guard against this and just return the
		 * result, gcd(INT_MIN, -1) = 1.
		 */
		if (arg2 == -1)
			return 1;
	}

	/* Use the Euclidean algorithm to find the GCD */
	while (arg2 != 0)
	{
		swap = arg2;
		arg2 = arg1 % arg2;
		arg1 = swap;
	}

	/*
	 * Make sure the result is positive. (We know we don't have INT_MIN
	 * anymore).
	 */
	if (arg1 < 0)
		arg1 = -arg1;

	return arg1;
}

Datum
int4gcd(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	result = int4gcd_internal(arg1, arg2);

	PG_RETURN_INT32(result);
}

/*
 * Least Common Multiple
 */
Datum
int4lcm(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		gcd;
	int32		result;

	/*
	 * Handle lcm(x, 0) = lcm(0, x) = 0 as a special case.  This prevents a
	 * division-by-zero error below when x is zero, and an overflow error from
	 * the GCD computation when x = INT_MIN.
	 */
	if (arg1 == 0 || arg2 == 0)
		PG_RETURN_INT32(0);

	/* lcm(x, y) = abs(x / gcd(x, y) * y) */
	gcd = int4gcd_internal(arg1, arg2);
	arg1 = arg1 / gcd;

	if (unlikely(pg_mul_s32_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	/* If the result is INT_MIN, it cannot be represented. */
	if (unlikely(result == PG_INT32_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	if (result < 0)
		result = -result;

	PG_RETURN_INT32(result);
}

Datum
int2larger(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_INT16((arg1 > arg2) ? arg1 : arg2);
}

Datum
int2smaller(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_INT16((arg1 < arg2) ? arg1 : arg2);
}

Datum
int4larger(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32((arg1 > arg2) ? arg1 : arg2);
}

Datum
int4smaller(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32((arg1 < arg2) ? arg1 : arg2);
}

/*
 * Bit-pushing operators
 *
 *		int[24]and		- returns arg1 & arg2
 *		int[24]or		- returns arg1 | arg2
 *		int[24]xor		- returns arg1 # arg2
 *		int[24]not		- returns ~arg1
 *		int[24]shl		- returns arg1 << arg2
 *		int[24]shr		- returns arg1 >> arg2
 */

Datum
int4and(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32(arg1 & arg2);
}

Datum
int4or(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32(arg1 | arg2);
}

Datum
int4xor(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32(arg1 ^ arg2);
}

Datum
int4shl(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32(arg1 << arg2);
}

Datum
int4shr(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32(arg1 >> arg2);
}

Datum
int4not(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);

	PG_RETURN_INT32(~arg1);
}

Datum
int2and(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_INT16(arg1 & arg2);
}

Datum
int2or(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_INT16(arg1 | arg2);
}

Datum
int2xor(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_INT16(arg1 ^ arg2);
}

Datum
int2not(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);

	PG_RETURN_INT16(~arg1);
}


Datum
int2shl(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT16(arg1 << arg2);
}

Datum
int2shr(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT16(arg1 >> arg2);
}

/*
 * non-persistent numeric series generator
 */
Datum
generate_series_int4(PG_FUNCTION_ARGS)
{
	return generate_series_step_int4(fcinfo);
}

Datum
generate_series_step_int4(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	generate_series_fctx *fctx;
	int32		result;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		int32		start = PG_GETARG_INT32(0);
		int32		finish = PG_GETARG_INT32(1);
		int32		step = 1;

		/* see if we were given an explicit step size */
		if (PG_NARGS() == 3)
			step = PG_GETARG_INT32(2);
		if (step == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot equal zero")));

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
		/*
		 * Increment current in preparation for next iteration. If next-value
		 * computation overflows, this is the final result.
		 */
		if (pg_add_s32_overflow(fctx->current, fctx->step, &fctx->current))
			fctx->step = 0;

		/* do when there is more left to send */
		SRF_RETURN_NEXT(funcctx, Int32GetDatum(result));
	}
	else
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
}

/*
 * Planner support function for generate_series(int4, int4 [, int4])
 */
Datum
generate_series_int4_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestRows))
	{
		/* Try to estimate the number of rows returned */
		SupportRequestRows *req = (SupportRequestRows *) rawreq;

		if (is_funcclause(req->node))	/* be paranoid */
		{
			List	   *args = ((FuncExpr *) req->node)->args;
			Node	   *arg1,
					   *arg2,
					   *arg3;

			/* We can use estimated argument values here */
			arg1 = estimate_expression_value(req->root, linitial(args));
			arg2 = estimate_expression_value(req->root, lsecond(args));
			if (list_length(args) >= 3)
				arg3 = estimate_expression_value(req->root, lthird(args));
			else
				arg3 = NULL;

			/*
			 * If any argument is constant NULL, we can safely assume that
			 * zero rows are returned.  Otherwise, if they're all non-NULL
			 * constants, we can calculate the number of rows that will be
			 * returned.  Use double arithmetic to avoid overflow hazards.
			 */
			if ((IsA(arg1, Const) &&
				 ((Const *) arg1)->constisnull) ||
				(IsA(arg2, Const) &&
				 ((Const *) arg2)->constisnull) ||
				(arg3 != NULL && IsA(arg3, Const) &&
				 ((Const *) arg3)->constisnull))
			{
				req->rows = 0;
				ret = (Node *) req;
			}
			else if (IsA(arg1, Const) &&
					 IsA(arg2, Const) &&
					 (arg3 == NULL || IsA(arg3, Const)))
			{
				double		start,
							finish,
							step;

				start = DatumGetInt32(((Const *) arg1)->constvalue);
				finish = DatumGetInt32(((Const *) arg2)->constvalue);
				step = arg3 ? DatumGetInt32(((Const *) arg3)->constvalue) : 1;

				/* This equation works for either sign of step */
				if (step != 0)
				{
					req->rows = floor((finish - start + step) / step);
					ret = (Node *) req;
				}
			}
		}
	}

	PG_RETURN_POINTER(ret);
}
