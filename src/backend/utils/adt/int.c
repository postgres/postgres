/*-------------------------------------------------------------------------
 *
 * int.c
 *	  Functions for the built-in integer types (except int8).
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/int.c,v 1.75 2006/10/04 00:29:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * OLD COMMENTS
 *		I/O routines:
 *		 int2in, int2out, int2recv, int2send
 *		 int4in, int4out, int4recv, int4send
 *		 int2vectorin, int2vectorout, int2vectorrecv, int2vectorsend
 *		Conversion routines:
 *		 itoi, int2_text, int4_text
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

#include "catalog/pg_type.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/builtins.h"


#define SAMESIGN(a,b)	(((a) < 0) == ((b) < 0))

#define Int2VectorSize(n)	(offsetof(int2vector, values) + (n) * sizeof(int2))

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

	PG_RETURN_INT16(pg_atoi(num, sizeof(int16), '\0'));
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
	pq_sendint(&buf, arg1, sizeof(int16));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * construct int2vector given a raw array of int2s
 *
 * If int2s is NULL then caller must fill values[] afterward
 */
int2vector *
buildint2vector(const int2 *int2s, int n)
{
	int2vector *result;

	result = (int2vector *) palloc0(Int2VectorSize(n));

	if (n > 0 && int2s)
		memcpy(result->values, int2s, n * sizeof(int2));

	/*
	 * Attach standard array header.  For historical reasons, we set the index
	 * lower bound to 0 not 1.
	 */
	result->size = Int2VectorSize(n);
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
	int2vector *result;
	int			n;

	result = (int2vector *) palloc0(Int2VectorSize(FUNC_MAX_ARGS));

	for (n = 0; *intString && n < FUNC_MAX_ARGS; n++)
	{
		if (sscanf(intString, "%hd", &result->values[n]) != 1)
			break;
		while (*intString && isspace((unsigned char) *intString))
			intString++;
		while (*intString && !isspace((unsigned char) *intString))
			intString++;
	}
	while (*intString && isspace((unsigned char) *intString))
		intString++;
	if (*intString)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("int2vector has too many elements")));

	result->size = Int2VectorSize(n);
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
		pg_itoa(int2Array->values[num], rp);
		while (*++rp != '\0')
			;
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
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	FunctionCallInfoData locfcinfo;
	int2vector *result;

	/*
	 * Normally one would call array_recv() using DirectFunctionCall3, but
	 * that does not work since array_recv wants to cache some data using
	 * fcinfo->flinfo->fn_extra.  So we need to pass it our own flinfo
	 * parameter.
	 */
	InitFunctionCallInfoData(locfcinfo, fcinfo->flinfo, 3, NULL, NULL);

	locfcinfo.arg[0] = PointerGetDatum(buf);
	locfcinfo.arg[1] = ObjectIdGetDatum(INT2OID);
	locfcinfo.arg[2] = Int32GetDatum(-1);
	locfcinfo.argnull[0] = false;
	locfcinfo.argnull[1] = false;
	locfcinfo.argnull[2] = false;

	result = (int2vector *) DatumGetPointer(array_recv(&locfcinfo));

	Assert(!locfcinfo.isnull);

	/* sanity checks: int2vector must be 1-D, no nulls */
	if (ARR_NDIM(result) != 1 ||
		ARR_HASNULL(result) ||
		ARR_ELEMTYPE(result) != INT2OID)
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

/*
 * We don't have a complete set of int2vector support routines,
 * but we need int2vectoreq for catcache indexing.
 */
Datum
int2vectoreq(PG_FUNCTION_ARGS)
{
	int2vector *a = (int2vector *) PG_GETARG_POINTER(0);
	int2vector *b = (int2vector *) PG_GETARG_POINTER(1);

	if (a->dim1 != b->dim1)
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(memcmp(a->values, b->values, a->dim1 * sizeof(int2)) == 0);
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

	PG_RETURN_INT32(pg_atoi(num, sizeof(int32), '\0'));
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
	pq_sendint(&buf, arg1, sizeof(int32));
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

	if (arg1 < SHRT_MIN || arg1 > SHRT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) arg1);
}

Datum
int2_text(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	text	   *result = (text *) palloc(7 + VARHDRSZ); /* sign,5 digits, '\0' */

	pg_itoa(arg1, VARDATA(result));
	VARATT_SIZEP(result) = strlen(VARDATA(result)) + VARHDRSZ;
	PG_RETURN_TEXT_P(result);
}

Datum
text_int2(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	Datum		result;
	int			len;
	char	   *str;

	len = VARSIZE(string) - VARHDRSZ;

	str = palloc(len + 1);
	memcpy(str, VARDATA(string), len);
	*(str + len) = '\0';

	result = DirectFunctionCall1(int2in, CStringGetDatum(str));
	pfree(str);

	return result;
}

Datum
int4_text(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	text	   *result = (text *) palloc(12 + VARHDRSZ);		/* sign,10 digits,'\0' */

	pg_ltoa(arg1, VARDATA(result));
	VARATT_SIZEP(result) = strlen(VARDATA(result)) + VARHDRSZ;
	PG_RETURN_TEXT_P(result);
}

Datum
text_int4(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	Datum		result;
	int			len;
	char	   *str;

	len = VARSIZE(string) - VARHDRSZ;

	str = palloc(len + 1);
	memcpy(str, VARDATA(string), len);
	*(str + len) = '\0';

	result = DirectFunctionCall1(int4in, CStringGetDatum(str));
	pfree(str);

	return result;
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
	int32		result;

	result = -arg;
	/* overflow check (needed for INT_MIN) */
	if (arg != 0 && SAMESIGN(result, arg))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
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

	result = arg1 + arg2;

	/*
	 * Overflow check.	If the inputs are of different signs then their sum
	 * cannot overflow.  If the inputs are of the same sign, their sum had
	 * better be that sign too.
	 */
	if (SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
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

	result = arg1 - arg2;

	/*
	 * Overflow check.	If the inputs are of the same sign then their
	 * difference cannot overflow.	If they are of different signs then the
	 * result should be of the same sign as the first input.
	 */
	if (!SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
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

#ifdef WIN32

	/*
	 * Win32 doesn't throw a catchable exception for SELECT -2147483648 *
	 * (-1);  -- INT_MIN
	 */
	if (arg2 == -1 && arg1 == INT_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
#endif

	result = arg1 * arg2;

	/*
	 * Overflow check.	We basically check to see if result / arg2 gives arg1
	 * again.  There are two cases where this fails: arg2 = 0 (which cannot
	 * overflow) and arg1 = INT_MIN, arg2 = -1 (where the division itself will
	 * overflow and thus incorrectly match).
	 *
	 * Since the division is likely much more expensive than the actual
	 * multiplication, we'd like to skip it where possible.  The best bang for
	 * the buck seems to be to check whether both inputs are in the int16
	 * range; if so, no overflow is possible.
	 */
	if (!(arg1 >= (int32) SHRT_MIN && arg1 <= (int32) SHRT_MAX &&
		  arg2 >= (int32) SHRT_MIN && arg2 <= (int32) SHRT_MAX) &&
		arg2 != 0 &&
		(result / arg2 != arg1 || (arg2 == -1 && arg1 < 0 && result < 0)))
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
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

#ifdef WIN32

	/*
	 * Win32 doesn't throw a catchable exception for SELECT -2147483648 /
	 * (-1); -- INT_MIN
	 */
	if (arg2 == -1 && arg1 == INT_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
#endif

	result = arg1 / arg2;

	/*
	 * Overflow check.	The only possible overflow case is for arg1 = INT_MIN,
	 * arg2 = -1, where the correct result is -INT_MIN, which can't be
	 * represented on a two's-complement machine.
	 */
	if (arg2 == -1 && arg1 < 0 && result < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int4inc(PG_FUNCTION_ARGS)
{
	int32		arg = PG_GETARG_INT32(0);
	int32		result;

	result = arg + 1;
	/* Overflow check */
	if (arg > 0 && result < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32(result);
}

Datum
int2um(PG_FUNCTION_ARGS)
{
	int16		arg = PG_GETARG_INT16(0);
	int16		result;

	result = -arg;
	/* overflow check (needed for SHRT_MIN) */
	if (arg != 0 && SAMESIGN(result, arg))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));
	PG_RETURN_INT16(result);
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

	result = arg1 + arg2;

	/*
	 * Overflow check.	If the inputs are of different signs then their sum
	 * cannot overflow.  If the inputs are of the same sign, their sum had
	 * better be that sign too.
	 */
	if (SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
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

	result = arg1 - arg2;

	/*
	 * Overflow check.	If the inputs are of the same sign then their
	 * difference cannot overflow.	If they are of different signs then the
	 * result should be of the same sign as the first input.
	 */
	if (!SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
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
	int32		result32;

	/*
	 * The most practical way to detect overflow is to do the arithmetic in
	 * int32 (so that the result can't overflow) and then do a range check.
	 */
	result32 = (int32) arg1 *(int32) arg2;

	if (result32 < SHRT_MIN || result32 > SHRT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) result32);
}

Datum
int2div(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int16		result;

	if (arg2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = arg1 / arg2;

	/*
	 * Overflow check.	The only possible overflow case is for arg1 =
	 * SHRT_MIN, arg2 = -1, where the correct result is -SHRT_MIN, which can't
	 * be represented on a two's-complement machine.
	 */
	if (arg2 == -1 && arg1 < 0 && result < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));
	PG_RETURN_INT16(result);
}

Datum
int24pl(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	result = arg1 + arg2;

	/*
	 * Overflow check.	If the inputs are of different signs then their sum
	 * cannot overflow.  If the inputs are of the same sign, their sum had
	 * better be that sign too.
	 */
	if (SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
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

	result = arg1 - arg2;

	/*
	 * Overflow check.	If the inputs are of the same sign then their
	 * difference cannot overflow.	If they are of different signs then the
	 * result should be of the same sign as the first input.
	 */
	if (!SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
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

	result = arg1 * arg2;

	/*
	 * Overflow check.	We basically check to see if result / arg2 gives arg1
	 * again.  There is one case where this fails: arg2 = 0 (which cannot
	 * overflow).
	 *
	 * Since the division is likely much more expensive than the actual
	 * multiplication, we'd like to skip it where possible.  The best bang for
	 * the buck seems to be to check whether both inputs are in the int16
	 * range; if so, no overflow is possible.
	 */
	if (!(arg2 >= (int32) SHRT_MIN && arg2 <= (int32) SHRT_MAX) &&
		result / arg2 != arg1)
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

	if (arg2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
	/* No overflow is possible */
	PG_RETURN_INT32((int32) arg1 / arg2);
}

Datum
int42pl(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int32		result;

	result = arg1 + arg2;

	/*
	 * Overflow check.	If the inputs are of different signs then their sum
	 * cannot overflow.  If the inputs are of the same sign, their sum had
	 * better be that sign too.
	 */
	if (SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
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

	result = arg1 - arg2;

	/*
	 * Overflow check.	If the inputs are of the same sign then their
	 * difference cannot overflow.	If they are of different signs then the
	 * result should be of the same sign as the first input.
	 */
	if (!SAMESIGN(arg1, arg2) && !SAMESIGN(result, arg1))
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

	result = arg1 * arg2;

	/*
	 * Overflow check.	We basically check to see if result / arg1 gives arg2
	 * again.  There is one case where this fails: arg1 = 0 (which cannot
	 * overflow).
	 *
	 * Since the division is likely much more expensive than the actual
	 * multiplication, we'd like to skip it where possible.  The best bang for
	 * the buck seems to be to check whether both inputs are in the int16
	 * range; if so, no overflow is possible.
	 */
	if (!(arg1 >= (int32) SHRT_MIN && arg1 <= (int32) SHRT_MAX) &&
		result / arg1 != arg2)
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

	if (arg2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = arg1 / arg2;

	/*
	 * Overflow check.	The only possible overflow case is for arg1 = INT_MIN,
	 * arg2 = -1, where the correct result is -INT_MIN, which can't be
	 * represented on a two's-complement machine.
	 */
	if (arg2 == -1 && arg1 < 0 && result < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int4mod(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	if (arg2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
	/* No overflow is possible */

	PG_RETURN_INT32(arg1 % arg2);
}

Datum
int2mod(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	if (arg2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
	/* No overflow is possible */

	PG_RETURN_INT16(arg1 % arg2);
}

Datum
int24mod(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	if (arg2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
	/* No overflow is possible */

	PG_RETURN_INT32(arg1 % arg2);
}

Datum
int42mod(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	if (arg2 == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
	/* No overflow is possible */

	PG_RETURN_INT32(arg1 % arg2);
}


/* int[24]abs()
 * Absolute value
 */
Datum
int4abs(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		result;

	result = (arg1 < 0) ? -arg1 : arg1;
	/* overflow check (needed for INT_MIN) */
	if (result < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int2abs(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		result;

	result = (arg1 < 0) ? -arg1 : arg1;
	/* overflow check (needed for SHRT_MIN) */
	if (result < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));
	PG_RETURN_INT16(result);
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
		SRF_RETURN_NEXT(funcctx, Int32GetDatum(result));
	}
	else
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
}
