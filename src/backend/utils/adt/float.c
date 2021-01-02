/*-------------------------------------------------------------------------
 *
 * float.c
 *	  Functions for the built-in floating-point types.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/float.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <limits.h>

#include "catalog/pg_type.h"
#include "common/int.h"
#include "common/shortest_dec.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/sortsupport.h"
#include "utils/timestamp.h"


/*
 * Configurable GUC parameter
 *
 * If >0, use shortest-decimal format for output; this is both the default and
 * allows for compatibility with clients that explicitly set a value here to
 * get round-trip-accurate results. If 0 or less, then use the old, slow,
 * decimal rounding method.
 */
int			extra_float_digits = 1;

/* Cached constants for degree-based trig functions */
static bool degree_consts_set = false;
static float8 sin_30 = 0;
static float8 one_minus_cos_60 = 0;
static float8 asin_0_5 = 0;
static float8 acos_0_5 = 0;
static float8 atan_1_0 = 0;
static float8 tan_45 = 0;
static float8 cot_45 = 0;

/*
 * These are intentionally not static; don't "fix" them.  They will never
 * be referenced by other files, much less changed; but we don't want the
 * compiler to know that, else it might try to precompute expressions
 * involving them.  See comments for init_degree_constants().
 */
float8		degree_c_thirty = 30.0;
float8		degree_c_forty_five = 45.0;
float8		degree_c_sixty = 60.0;
float8		degree_c_one_half = 0.5;
float8		degree_c_one = 1.0;

/* State for drandom() and setseed() */
static bool drandom_seed_set = false;
static unsigned short drandom_seed[3] = {0, 0, 0};

/* Local function prototypes */
static double sind_q1(double x);
static double cosd_q1(double x);
static void init_degree_constants(void);


/*
 * We use these out-of-line ereport() calls to report float overflow,
 * underflow, and zero-divide, because following our usual practice of
 * repeating them at each call site would lead to a lot of code bloat.
 *
 * This does mean that you don't get a useful error location indicator.
 */
pg_noinline void
float_overflow_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value out of range: overflow")));
}

pg_noinline void
float_underflow_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value out of range: underflow")));
}

pg_noinline void
float_zero_divide_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_DIVISION_BY_ZERO),
			 errmsg("division by zero")));
}


/*
 * Returns -1 if 'val' represents negative infinity, 1 if 'val'
 * represents (positive) infinity, and 0 otherwise. On some platforms,
 * this is equivalent to the isinf() macro, but not everywhere: C99
 * does not specify that isinf() needs to distinguish between positive
 * and negative infinity.
 */
int
is_infinite(double val)
{
	int			inf = isinf(val);

	if (inf == 0)
		return 0;
	else if (val > 0)
		return 1;
	else
		return -1;
}


/* ========== USER I/O ROUTINES ========== */


/*
 *		float4in		- converts "num" to float4
 *
 * Note that this code now uses strtof(), where it used to use strtod().
 *
 * The motivation for using strtof() is to avoid a double-rounding problem:
 * for certain decimal inputs, if you round the input correctly to a double,
 * and then round the double to a float, the result is incorrect in that it
 * does not match the result of rounding the decimal value to float directly.
 *
 * One of the best examples is 7.038531e-26:
 *
 * 0xAE43FDp-107 = 7.03853069185120912085...e-26
 *      midpoint   7.03853100000000022281...e-26
 * 0xAE43FEp-107 = 7.03853130814879132477...e-26
 *
 * making 0xAE43FDp-107 the correct float result, but if you do the conversion
 * via a double, you get
 *
 * 0xAE43FD.7FFFFFF8p-107 = 7.03853099999999907487...e-26
 *               midpoint   7.03853099999999964884...e-26
 * 0xAE43FD.80000000p-107 = 7.03853100000000022281...e-26
 * 0xAE43FD.80000008p-107 = 7.03853100000000137076...e-26
 *
 * so the value rounds to the double exactly on the midpoint between the two
 * nearest floats, and then rounding again to a float gives the incorrect
 * result of 0xAE43FEp-107.
 *
 */
Datum
float4in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);
	char	   *orig_num;
	float		val;
	char	   *endptr;

	/*
	 * endptr points to the first character _after_ the sequence we recognized
	 * as a valid floating point number. orig_num points to the original input
	 * string.
	 */
	orig_num = num;

	/* skip leading whitespace */
	while (*num != '\0' && isspace((unsigned char) *num))
		num++;

	/*
	 * Check for an empty-string input to begin with, to avoid the vagaries of
	 * strtod() on different platforms.
	 */
	if (*num == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"real", orig_num)));

	errno = 0;
	val = strtof(num, &endptr);

	/* did we not see anything that looks like a double? */
	if (endptr == num || errno != 0)
	{
		int			save_errno = errno;

		/*
		 * C99 requires that strtof() accept NaN, [+-]Infinity, and [+-]Inf,
		 * but not all platforms support all of these (and some accept them
		 * but set ERANGE anyway...)  Therefore, we check for these inputs
		 * ourselves if strtof() fails.
		 *
		 * Note: C99 also requires hexadecimal input as well as some extended
		 * forms of NaN, but we consider these forms unportable and don't try
		 * to support them.  You can use 'em if your strtof() takes 'em.
		 */
		if (pg_strncasecmp(num, "NaN", 3) == 0)
		{
			val = get_float4_nan();
			endptr = num + 3;
		}
		else if (pg_strncasecmp(num, "Infinity", 8) == 0)
		{
			val = get_float4_infinity();
			endptr = num + 8;
		}
		else if (pg_strncasecmp(num, "+Infinity", 9) == 0)
		{
			val = get_float4_infinity();
			endptr = num + 9;
		}
		else if (pg_strncasecmp(num, "-Infinity", 9) == 0)
		{
			val = -get_float4_infinity();
			endptr = num + 9;
		}
		else if (pg_strncasecmp(num, "inf", 3) == 0)
		{
			val = get_float4_infinity();
			endptr = num + 3;
		}
		else if (pg_strncasecmp(num, "+inf", 4) == 0)
		{
			val = get_float4_infinity();
			endptr = num + 4;
		}
		else if (pg_strncasecmp(num, "-inf", 4) == 0)
		{
			val = -get_float4_infinity();
			endptr = num + 4;
		}
		else if (save_errno == ERANGE)
		{
			/*
			 * Some platforms return ERANGE for denormalized numbers (those
			 * that are not zero, but are too close to zero to have full
			 * precision).  We'd prefer not to throw error for that, so try to
			 * detect whether it's a "real" out-of-range condition by checking
			 * to see if the result is zero or huge.
			 *
			 * Use isinf() rather than HUGE_VALF on VS2013 because it
			 * generates a spurious overflow warning for -HUGE_VALF.  Also use
			 * isinf() if HUGE_VALF is missing.
			 */
			if (val == 0.0 ||
#if !defined(HUGE_VALF) || (defined(_MSC_VER) && (_MSC_VER < 1900))
				isinf(val)
#else
				(val >= HUGE_VALF || val <= -HUGE_VALF)
#endif
				)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("\"%s\" is out of range for type real",
								orig_num)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							"real", orig_num)));
	}

	/* skip trailing whitespace */
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;

	/* if there is any junk left at the end of the string, bail out */
	if (*endptr != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"real", orig_num)));

	PG_RETURN_FLOAT4(val);
}

/*
 *		float4out		- converts a float4 number to a string
 *						  using a standard output format
 */
Datum
float4out(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);
	char	   *ascii = (char *) palloc(32);
	int			ndig = FLT_DIG + extra_float_digits;

	if (extra_float_digits > 0)
	{
		float_to_shortest_decimal_buf(num, ascii);
		PG_RETURN_CSTRING(ascii);
	}

	(void) pg_strfromd(ascii, 32, ndig, num);
	PG_RETURN_CSTRING(ascii);
}

/*
 *		float4recv			- converts external binary format to float4
 */
Datum
float4recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_FLOAT4(pq_getmsgfloat4(buf));
}

/*
 *		float4send			- converts float4 to binary format
 */
Datum
float4send(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendfloat4(&buf, num);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 *		float8in		- converts "num" to float8
 */
Datum
float8in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);

	PG_RETURN_FLOAT8(float8in_internal(num, NULL, "double precision", num));
}

/* Convenience macro: set *have_error flag (if provided) or throw error */
#define RETURN_ERROR(throw_error, have_error) \
do { \
	if (have_error) { \
		*have_error = true; \
		return 0.0; \
	} else { \
		throw_error; \
	} \
} while (0)

/*
 * float8in_internal_opt_error - guts of float8in()
 *
 * This is exposed for use by functions that want a reasonably
 * platform-independent way of inputting doubles.  The behavior is
 * essentially like strtod + ereport on error, but note the following
 * differences:
 * 1. Both leading and trailing whitespace are skipped.
 * 2. If endptr_p is NULL, we throw error if there's trailing junk.
 * Otherwise, it's up to the caller to complain about trailing junk.
 * 3. In event of a syntax error, the report mentions the given type_name
 * and prints orig_string as the input; this is meant to support use of
 * this function with types such as "box" and "point", where what we are
 * parsing here is just a substring of orig_string.
 *
 * "num" could validly be declared "const char *", but that results in an
 * unreasonable amount of extra casting both here and in callers, so we don't.
 *
 * When "*have_error" flag is provided, it's set instead of throwing an
 * error.  This is helpful when caller need to handle errors by itself.
 */
double
float8in_internal_opt_error(char *num, char **endptr_p,
							const char *type_name, const char *orig_string,
							bool *have_error)
{
	double		val;
	char	   *endptr;

	if (have_error)
		*have_error = false;

	/* skip leading whitespace */
	while (*num != '\0' && isspace((unsigned char) *num))
		num++;

	/*
	 * Check for an empty-string input to begin with, to avoid the vagaries of
	 * strtod() on different platforms.
	 */
	if (*num == '\0')
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							  errmsg("invalid input syntax for type %s: \"%s\"",
									 type_name, orig_string))),
					 have_error);

	errno = 0;
	val = strtod(num, &endptr);

	/* did we not see anything that looks like a double? */
	if (endptr == num || errno != 0)
	{
		int			save_errno = errno;

		/*
		 * C99 requires that strtod() accept NaN, [+-]Infinity, and [+-]Inf,
		 * but not all platforms support all of these (and some accept them
		 * but set ERANGE anyway...)  Therefore, we check for these inputs
		 * ourselves if strtod() fails.
		 *
		 * Note: C99 also requires hexadecimal input as well as some extended
		 * forms of NaN, but we consider these forms unportable and don't try
		 * to support them.  You can use 'em if your strtod() takes 'em.
		 */
		if (pg_strncasecmp(num, "NaN", 3) == 0)
		{
			val = get_float8_nan();
			endptr = num + 3;
		}
		else if (pg_strncasecmp(num, "Infinity", 8) == 0)
		{
			val = get_float8_infinity();
			endptr = num + 8;
		}
		else if (pg_strncasecmp(num, "+Infinity", 9) == 0)
		{
			val = get_float8_infinity();
			endptr = num + 9;
		}
		else if (pg_strncasecmp(num, "-Infinity", 9) == 0)
		{
			val = -get_float8_infinity();
			endptr = num + 9;
		}
		else if (pg_strncasecmp(num, "inf", 3) == 0)
		{
			val = get_float8_infinity();
			endptr = num + 3;
		}
		else if (pg_strncasecmp(num, "+inf", 4) == 0)
		{
			val = get_float8_infinity();
			endptr = num + 4;
		}
		else if (pg_strncasecmp(num, "-inf", 4) == 0)
		{
			val = -get_float8_infinity();
			endptr = num + 4;
		}
		else if (save_errno == ERANGE)
		{
			/*
			 * Some platforms return ERANGE for denormalized numbers (those
			 * that are not zero, but are too close to zero to have full
			 * precision).  We'd prefer not to throw error for that, so try to
			 * detect whether it's a "real" out-of-range condition by checking
			 * to see if the result is zero or huge.
			 *
			 * On error, we intentionally complain about double precision not
			 * the given type name, and we print only the part of the string
			 * that is the current number.
			 */
			if (val == 0.0 || val >= HUGE_VAL || val <= -HUGE_VAL)
			{
				char	   *errnumber = pstrdup(num);

				errnumber[endptr - num] = '\0';
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
									  errmsg("\"%s\" is out of range for type double precision",
											 errnumber))),
							 have_error);
			}
		}
		else
			RETURN_ERROR(ereport(ERROR,
								 (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								  errmsg("invalid input syntax for type "
										 "%s: \"%s\"",
										 type_name, orig_string))),
						 have_error);
	}

	/* skip trailing whitespace */
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;

	/* report stopping point if wanted, else complain if not end of string */
	if (endptr_p)
		*endptr_p = endptr;
	else if (*endptr != '\0')
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							  errmsg("invalid input syntax for type "
									 "%s: \"%s\"",
									 type_name, orig_string))),
					 have_error);

	return val;
}

/*
 * Interface to float8in_internal_opt_error() without "have_error" argument.
 */
double
float8in_internal(char *num, char **endptr_p,
				  const char *type_name, const char *orig_string)
{
	return float8in_internal_opt_error(num, endptr_p, type_name,
									   orig_string, NULL);
}


/*
 *		float8out		- converts float8 number to a string
 *						  using a standard output format
 */
Datum
float8out(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);

	PG_RETURN_CSTRING(float8out_internal(num));
}

/*
 * float8out_internal - guts of float8out()
 *
 * This is exposed for use by functions that want a reasonably
 * platform-independent way of outputting doubles.
 * The result is always palloc'd.
 */
char *
float8out_internal(double num)
{
	char	   *ascii = (char *) palloc(32);
	int			ndig = DBL_DIG + extra_float_digits;

	if (extra_float_digits > 0)
	{
		double_to_shortest_decimal_buf(num, ascii);
		return ascii;
	}

	(void) pg_strfromd(ascii, 32, ndig, num);
	return ascii;
}

/*
 *		float8recv			- converts external binary format to float8
 */
Datum
float8recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_FLOAT8(pq_getmsgfloat8(buf));
}

/*
 *		float8send			- converts float8 to binary format
 */
Datum
float8send(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendfloat8(&buf, num);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/* ========== PUBLIC ROUTINES ========== */


/*
 *		======================
 *		FLOAT4 BASE OPERATIONS
 *		======================
 */

/*
 *		float4abs		- returns |arg1| (absolute value)
 */
Datum
float4abs(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);

	PG_RETURN_FLOAT4((float4) fabs(arg1));
}

/*
 *		float4um		- returns -arg1 (unary minus)
 */
Datum
float4um(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		result;

	result = -arg1;
	PG_RETURN_FLOAT4(result);
}

Datum
float4up(PG_FUNCTION_ARGS)
{
	float4		arg = PG_GETARG_FLOAT4(0);

	PG_RETURN_FLOAT4(arg);
}

Datum
float4larger(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);
	float4		result;

	if (float4_gt(arg1, arg2))
		result = arg1;
	else
		result = arg2;
	PG_RETURN_FLOAT4(result);
}

Datum
float4smaller(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);
	float4		result;

	if (float4_lt(arg1, arg2))
		result = arg1;
	else
		result = arg2;
	PG_RETURN_FLOAT4(result);
}

/*
 *		======================
 *		FLOAT8 BASE OPERATIONS
 *		======================
 */

/*
 *		float8abs		- returns |arg1| (absolute value)
 */
Datum
float8abs(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(fabs(arg1));
}


/*
 *		float8um		- returns -arg1 (unary minus)
 */
Datum
float8um(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	result = -arg1;
	PG_RETURN_FLOAT8(result);
}

Datum
float8up(PG_FUNCTION_ARGS)
{
	float8		arg = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(arg);
}

Datum
float8larger(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	if (float8_gt(arg1, arg2))
		result = arg1;
	else
		result = arg2;
	PG_RETURN_FLOAT8(result);
}

Datum
float8smaller(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	if (float8_lt(arg1, arg2))
		result = arg1;
	else
		result = arg2;
	PG_RETURN_FLOAT8(result);
}


/*
 *		====================
 *		ARITHMETIC OPERATORS
 *		====================
 */

/*
 *		float4pl		- returns arg1 + arg2
 *		float4mi		- returns arg1 - arg2
 *		float4mul		- returns arg1 * arg2
 *		float4div		- returns arg1 / arg2
 */
Datum
float4pl(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT4(float4_pl(arg1, arg2));
}

Datum
float4mi(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT4(float4_mi(arg1, arg2));
}

Datum
float4mul(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT4(float4_mul(arg1, arg2));
}

Datum
float4div(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT4(float4_div(arg1, arg2));
}

/*
 *		float8pl		- returns arg1 + arg2
 *		float8mi		- returns arg1 - arg2
 *		float8mul		- returns arg1 * arg2
 *		float8div		- returns arg1 / arg2
 */
Datum
float8pl(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_pl(arg1, arg2));
}

Datum
float8mi(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_mi(arg1, arg2));
}

Datum
float8mul(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_mul(arg1, arg2));
}

Datum
float8div(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_div(arg1, arg2));
}


/*
 *		====================
 *		COMPARISON OPERATORS
 *		====================
 */

/*
 *		float4{eq,ne,lt,le,gt,ge}		- float4/float4 comparison operations
 */
int
float4_cmp_internal(float4 a, float4 b)
{
	if (float4_gt(a, b))
		return 1;
	if (float4_lt(a, b))
		return -1;
	return 0;
}

Datum
float4eq(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_eq(arg1, arg2));
}

Datum
float4ne(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_ne(arg1, arg2));
}

Datum
float4lt(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_lt(arg1, arg2));
}

Datum
float4le(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_le(arg1, arg2));
}

Datum
float4gt(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_gt(arg1, arg2));
}

Datum
float4ge(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_ge(arg1, arg2));
}

Datum
btfloat4cmp(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_INT32(float4_cmp_internal(arg1, arg2));
}

static int
btfloat4fastcmp(Datum x, Datum y, SortSupport ssup)
{
	float4		arg1 = DatumGetFloat4(x);
	float4		arg2 = DatumGetFloat4(y);

	return float4_cmp_internal(arg1, arg2);
}

Datum
btfloat4sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = btfloat4fastcmp;
	PG_RETURN_VOID();
}

/*
 *		float8{eq,ne,lt,le,gt,ge}		- float8/float8 comparison operations
 */
int
float8_cmp_internal(float8 a, float8 b)
{
	if (float8_gt(a, b))
		return 1;
	if (float8_lt(a, b))
		return -1;
	return 0;
}

Datum
float8eq(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_eq(arg1, arg2));
}

Datum
float8ne(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_ne(arg1, arg2));
}

Datum
float8lt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_lt(arg1, arg2));
}

Datum
float8le(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_le(arg1, arg2));
}

Datum
float8gt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_gt(arg1, arg2));
}

Datum
float8ge(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_ge(arg1, arg2));
}

Datum
btfloat8cmp(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_INT32(float8_cmp_internal(arg1, arg2));
}

static int
btfloat8fastcmp(Datum x, Datum y, SortSupport ssup)
{
	float8		arg1 = DatumGetFloat8(x);
	float8		arg2 = DatumGetFloat8(y);

	return float8_cmp_internal(arg1, arg2);
}

Datum
btfloat8sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = btfloat8fastcmp;
	PG_RETURN_VOID();
}

Datum
btfloat48cmp(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	/* widen float4 to float8 and then compare */
	PG_RETURN_INT32(float8_cmp_internal(arg1, arg2));
}

Datum
btfloat84cmp(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	/* widen float4 to float8 and then compare */
	PG_RETURN_INT32(float8_cmp_internal(arg1, arg2));
}

/*
 * in_range support function for float8.
 *
 * Note: we needn't supply a float8_float4 variant, as implicit coercion
 * of the offset value takes care of that scenario just as well.
 */
Datum
in_range_float8_float8(PG_FUNCTION_ARGS)
{
	float8		val = PG_GETARG_FLOAT8(0);
	float8		base = PG_GETARG_FLOAT8(1);
	float8		offset = PG_GETARG_FLOAT8(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	float8		sum;

	/*
	 * Reject negative or NaN offset.  Negative is per spec, and NaN is
	 * because appropriate semantics for that seem non-obvious.
	 */
	if (isnan(offset) || offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/*
	 * Deal with cases where val and/or base is NaN, following the rule that
	 * NaN sorts after non-NaN (cf float8_cmp_internal).  The offset cannot
	 * affect the conclusion.
	 */
	if (isnan(val))
	{
		if (isnan(base))
			PG_RETURN_BOOL(true);	/* NAN = NAN */
		else
			PG_RETURN_BOOL(!less);	/* NAN > non-NAN */
	}
	else if (isnan(base))
	{
		PG_RETURN_BOOL(less);	/* non-NAN < NAN */
	}

	/*
	 * Deal with cases where both base and offset are infinite, and computing
	 * base +/- offset would produce NaN.  This corresponds to a window frame
	 * whose boundary infinitely precedes +inf or infinitely follows -inf,
	 * which is not well-defined.  For consistency with other cases involving
	 * infinities, such as the fact that +inf infinitely follows +inf, we
	 * choose to assume that +inf infinitely precedes +inf and -inf infinitely
	 * follows -inf, and therefore that all finite and infinite values are in
	 * such a window frame.
	 *
	 * offset is known positive, so we need only check the sign of base in
	 * this test.
	 */
	if (isinf(offset) && isinf(base) &&
		(sub ? base > 0 : base < 0))
		PG_RETURN_BOOL(true);

	/*
	 * Otherwise it should be safe to compute base +/- offset.  We trust the
	 * FPU to cope if an input is +/-inf or the true sum would overflow, and
	 * produce a suitably signed infinity, which will compare properly against
	 * val whether or not that's infinity.
	 */
	if (sub)
		sum = base - offset;
	else
		sum = base + offset;

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

/*
 * in_range support function for float4.
 *
 * We would need a float4_float8 variant in any case, so we supply that and
 * let implicit coercion take care of the float4_float4 case.
 */
Datum
in_range_float4_float8(PG_FUNCTION_ARGS)
{
	float4		val = PG_GETARG_FLOAT4(0);
	float4		base = PG_GETARG_FLOAT4(1);
	float8		offset = PG_GETARG_FLOAT8(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	float8		sum;

	/*
	 * Reject negative or NaN offset.  Negative is per spec, and NaN is
	 * because appropriate semantics for that seem non-obvious.
	 */
	if (isnan(offset) || offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/*
	 * Deal with cases where val and/or base is NaN, following the rule that
	 * NaN sorts after non-NaN (cf float8_cmp_internal).  The offset cannot
	 * affect the conclusion.
	 */
	if (isnan(val))
	{
		if (isnan(base))
			PG_RETURN_BOOL(true);	/* NAN = NAN */
		else
			PG_RETURN_BOOL(!less);	/* NAN > non-NAN */
	}
	else if (isnan(base))
	{
		PG_RETURN_BOOL(less);	/* non-NAN < NAN */
	}

	/*
	 * Deal with cases where both base and offset are infinite, and computing
	 * base +/- offset would produce NaN.  This corresponds to a window frame
	 * whose boundary infinitely precedes +inf or infinitely follows -inf,
	 * which is not well-defined.  For consistency with other cases involving
	 * infinities, such as the fact that +inf infinitely follows +inf, we
	 * choose to assume that +inf infinitely precedes +inf and -inf infinitely
	 * follows -inf, and therefore that all finite and infinite values are in
	 * such a window frame.
	 *
	 * offset is known positive, so we need only check the sign of base in
	 * this test.
	 */
	if (isinf(offset) && isinf(base) &&
		(sub ? base > 0 : base < 0))
		PG_RETURN_BOOL(true);

	/*
	 * Otherwise it should be safe to compute base +/- offset.  We trust the
	 * FPU to cope if an input is +/-inf or the true sum would overflow, and
	 * produce a suitably signed infinity, which will compare properly against
	 * val whether or not that's infinity.
	 */
	if (sub)
		sum = base - offset;
	else
		sum = base + offset;

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}


/*
 *		===================
 *		CONVERSION ROUTINES
 *		===================
 */

/*
 *		ftod			- converts a float4 number to a float8 number
 */
Datum
ftod(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);

	PG_RETURN_FLOAT8((float8) num);
}


/*
 *		dtof			- converts a float8 number to a float4 number
 */
Datum
dtof(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);
	float4		result;

	result = (float4) num;
	if (unlikely(isinf(result)) && !isinf(num))
		float_overflow_error();
	if (unlikely(result == 0.0f) && num != 0.0)
		float_underflow_error();

	PG_RETURN_FLOAT4(result);
}


/*
 *		dtoi4			- converts a float8 number to an int4 number
 */
Datum
dtoi4(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);

	/*
	 * Get rid of any fractional part in the input.  This is so we don't fail
	 * on just-out-of-range values that would round into range.  Note
	 * assumption that rint() will pass through a NaN or Inf unchanged.
	 */
	num = rint(num);

	/* Range check */
	if (unlikely(isnan(num) || !FLOAT8_FITS_IN_INT32(num)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32((int32) num);
}


/*
 *		dtoi2			- converts a float8 number to an int2 number
 */
Datum
dtoi2(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);

	/*
	 * Get rid of any fractional part in the input.  This is so we don't fail
	 * on just-out-of-range values that would round into range.  Note
	 * assumption that rint() will pass through a NaN or Inf unchanged.
	 */
	num = rint(num);

	/* Range check */
	if (unlikely(isnan(num) || !FLOAT8_FITS_IN_INT16(num)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) num);
}


/*
 *		i4tod			- converts an int4 number to a float8 number
 */
Datum
i4tod(PG_FUNCTION_ARGS)
{
	int32		num = PG_GETARG_INT32(0);

	PG_RETURN_FLOAT8((float8) num);
}


/*
 *		i2tod			- converts an int2 number to a float8 number
 */
Datum
i2tod(PG_FUNCTION_ARGS)
{
	int16		num = PG_GETARG_INT16(0);

	PG_RETURN_FLOAT8((float8) num);
}


/*
 *		ftoi4			- converts a float4 number to an int4 number
 */
Datum
ftoi4(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);

	/*
	 * Get rid of any fractional part in the input.  This is so we don't fail
	 * on just-out-of-range values that would round into range.  Note
	 * assumption that rint() will pass through a NaN or Inf unchanged.
	 */
	num = rint(num);

	/* Range check */
	if (unlikely(isnan(num) || !FLOAT4_FITS_IN_INT32(num)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32((int32) num);
}


/*
 *		ftoi2			- converts a float4 number to an int2 number
 */
Datum
ftoi2(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);

	/*
	 * Get rid of any fractional part in the input.  This is so we don't fail
	 * on just-out-of-range values that would round into range.  Note
	 * assumption that rint() will pass through a NaN or Inf unchanged.
	 */
	num = rint(num);

	/* Range check */
	if (unlikely(isnan(num) || !FLOAT4_FITS_IN_INT16(num)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) num);
}


/*
 *		i4tof			- converts an int4 number to a float4 number
 */
Datum
i4tof(PG_FUNCTION_ARGS)
{
	int32		num = PG_GETARG_INT32(0);

	PG_RETURN_FLOAT4((float4) num);
}


/*
 *		i2tof			- converts an int2 number to a float4 number
 */
Datum
i2tof(PG_FUNCTION_ARGS)
{
	int16		num = PG_GETARG_INT16(0);

	PG_RETURN_FLOAT4((float4) num);
}


/*
 *		=======================
 *		RANDOM FLOAT8 OPERATORS
 *		=======================
 */

/*
 *		dround			- returns	ROUND(arg1)
 */
Datum
dround(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(rint(arg1));
}

/*
 *		dceil			- returns the smallest integer greater than or
 *						  equal to the specified float
 */
Datum
dceil(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(ceil(arg1));
}

/*
 *		dfloor			- returns the largest integer lesser than or
 *						  equal to the specified float
 */
Datum
dfloor(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(floor(arg1));
}

/*
 *		dsign			- returns -1 if the argument is less than 0, 0
 *						  if the argument is equal to 0, and 1 if the
 *						  argument is greater than zero.
 */
Datum
dsign(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	if (arg1 > 0)
		result = 1.0;
	else if (arg1 < 0)
		result = -1.0;
	else
		result = 0.0;

	PG_RETURN_FLOAT8(result);
}

/*
 *		dtrunc			- returns truncation-towards-zero of arg1,
 *						  arg1 >= 0 ... the greatest integer less
 *										than or equal to arg1
 *						  arg1 < 0	... the least integer greater
 *										than or equal to arg1
 */
Datum
dtrunc(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	if (arg1 >= 0)
		result = floor(arg1);
	else
		result = -floor(-arg1);

	PG_RETURN_FLOAT8(result);
}


/*
 *		dsqrt			- returns square root of arg1
 */
Datum
dsqrt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	if (arg1 < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
				 errmsg("cannot take square root of a negative number")));

	result = sqrt(arg1);
	if (unlikely(isinf(result)) && !isinf(arg1))
		float_overflow_error();
	if (unlikely(result == 0.0) && arg1 != 0.0)
		float_underflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dcbrt			- returns cube root of arg1
 */
Datum
dcbrt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	result = cbrt(arg1);
	if (unlikely(isinf(result)) && !isinf(arg1))
		float_overflow_error();
	if (unlikely(result == 0.0) && arg1 != 0.0)
		float_underflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dpow			- returns pow(arg1,arg2)
 */
Datum
dpow(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	/*
	 * The POSIX spec says that NaN ^ 0 = 1, and 1 ^ NaN = 1, while all other
	 * cases with NaN inputs yield NaN (with no error).  Many older platforms
	 * get one or more of these cases wrong, so deal with them via explicit
	 * logic rather than trusting pow(3).
	 */
	if (isnan(arg1))
	{
		if (isnan(arg2) || arg2 != 0.0)
			PG_RETURN_FLOAT8(get_float8_nan());
		PG_RETURN_FLOAT8(1.0);
	}
	if (isnan(arg2))
	{
		if (arg1 != 1.0)
			PG_RETURN_FLOAT8(get_float8_nan());
		PG_RETURN_FLOAT8(1.0);
	}

	/*
	 * The SQL spec requires that we emit a particular SQLSTATE error code for
	 * certain error conditions.  Specifically, we don't return a
	 * divide-by-zero error code for 0 ^ -1.
	 */
	if (arg1 == 0 && arg2 < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
				 errmsg("zero raised to a negative power is undefined")));
	if (arg1 < 0 && floor(arg2) != arg2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
				 errmsg("a negative number raised to a non-integer power yields a complex result")));

	/*
	 * We don't trust the platform's pow() to handle infinity cases per POSIX
	 * spec either, so deal with those explicitly too.  It's easier to handle
	 * infinite y first, so that it doesn't matter if x is also infinite.
	 */
	if (isinf(arg2))
	{
		float8		absx = fabs(arg1);

		if (absx == 1.0)
			result = 1.0;
		else if (arg2 > 0.0)	/* y = +Inf */
		{
			if (absx > 1.0)
				result = arg2;
			else
				result = 0.0;
		}
		else					/* y = -Inf */
		{
			if (absx > 1.0)
				result = 0.0;
			else
				result = -arg2;
		}
	}
	else if (isinf(arg1))
	{
		if (arg2 == 0.0)
			result = 1.0;
		else if (arg1 > 0.0)	/* x = +Inf */
		{
			if (arg2 > 0.0)
				result = arg1;
			else
				result = 0.0;
		}
		else					/* x = -Inf */
		{
			/*
			 * Per POSIX, the sign of the result depends on whether y is an
			 * odd integer.  Since x < 0, we already know from the previous
			 * domain check that y is an integer.  It is odd if y/2 is not
			 * also an integer.
			 */
			float8		halfy = arg2 / 2;	/* should be computed exactly */
			bool		yisoddinteger = (floor(halfy) != halfy);

			if (arg2 > 0.0)
				result = yisoddinteger ? arg1 : -arg1;
			else
				result = yisoddinteger ? -0.0 : 0.0;
		}
	}
	else
	{
		/*
		 * pow() sets errno on only some platforms, depending on whether it
		 * follows _IEEE_, _POSIX_, _XOPEN_, or _SVID_, so we must check both
		 * errno and invalid output values.  (We can't rely on just the
		 * latter, either; some old platforms return a large-but-finite
		 * HUGE_VAL when reporting overflow.)
		 */
		errno = 0;
		result = pow(arg1, arg2);
		if (errno == EDOM || isnan(result))
		{
			/*
			 * We handled all possible domain errors above, so this should be
			 * impossible.  However, old glibc versions on x86 have a bug that
			 * causes them to fail this way for abs(y) greater than 2^63:
			 *
			 * https://sourceware.org/bugzilla/show_bug.cgi?id=3866
			 *
			 * Hence, if we get here, assume y is finite but large (large
			 * enough to be certainly even). The result should be 0 if x == 0,
			 * 1.0 if abs(x) == 1.0, otherwise an overflow or underflow error.
			 */
			if (arg1 == 0.0)
				result = 0.0;	/* we already verified y is positive */
			else
			{
				float8		absx = fabs(arg1);

				if (absx == 1.0)
					result = 1.0;
				else if (arg2 >= 0.0 ? (absx > 1.0) : (absx < 1.0))
					float_overflow_error();
				else
					float_underflow_error();
			}
		}
		else if (errno == ERANGE)
		{
			if (result != 0.0)
				float_overflow_error();
			else
				float_underflow_error();
		}
		else
		{
			if (unlikely(isinf(result)))
				float_overflow_error();
			if (unlikely(result == 0.0) && arg1 != 0.0)
				float_underflow_error();
		}
	}

	PG_RETURN_FLOAT8(result);
}


/*
 *		dexp			- returns the exponential function of arg1
 */
Datum
dexp(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * Handle NaN and Inf cases explicitly.  This avoids needing to assume
	 * that the platform's exp() conforms to POSIX for these cases, and it
	 * removes some edge cases for the overflow checks below.
	 */
	if (isnan(arg1))
		result = arg1;
	else if (isinf(arg1))
	{
		/* Per POSIX, exp(-Inf) is 0 */
		result = (arg1 > 0.0) ? arg1 : 0;
	}
	else
	{
		/*
		 * On some platforms, exp() will not set errno but just return Inf or
		 * zero to report overflow/underflow; therefore, test both cases.
		 */
		errno = 0;
		result = exp(arg1);
		if (unlikely(errno == ERANGE))
		{
			if (result != 0.0)
				float_overflow_error();
			else
				float_underflow_error();
		}
		else if (unlikely(isinf(result)))
			float_overflow_error();
		else if (unlikely(result == 0.0))
			float_underflow_error();
	}

	PG_RETURN_FLOAT8(result);
}


/*
 *		dlog1			- returns the natural logarithm of arg1
 */
Datum
dlog1(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * Emit particular SQLSTATE error codes for ln(). This is required by the
	 * SQL standard.
	 */
	if (arg1 == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
				 errmsg("cannot take logarithm of zero")));
	if (arg1 < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
				 errmsg("cannot take logarithm of a negative number")));

	result = log(arg1);
	if (unlikely(isinf(result)) && !isinf(arg1))
		float_overflow_error();
	if (unlikely(result == 0.0) && arg1 != 1.0)
		float_underflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dlog10			- returns the base 10 logarithm of arg1
 */
Datum
dlog10(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * Emit particular SQLSTATE error codes for log(). The SQL spec doesn't
	 * define log(), but it does define ln(), so it makes sense to emit the
	 * same error code for an analogous error condition.
	 */
	if (arg1 == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
				 errmsg("cannot take logarithm of zero")));
	if (arg1 < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
				 errmsg("cannot take logarithm of a negative number")));

	result = log10(arg1);
	if (unlikely(isinf(result)) && !isinf(arg1))
		float_overflow_error();
	if (unlikely(result == 0.0) && arg1 != 1.0)
		float_underflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dacos			- returns the arccos of arg1 (radians)
 */
Datum
dacos(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* Per the POSIX spec, return NaN if the input is NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/*
	 * The principal branch of the inverse cosine function maps values in the
	 * range [-1, 1] to values in the range [0, Pi], so we should reject any
	 * inputs outside that range and the result will always be finite.
	 */
	if (arg1 < -1.0 || arg1 > 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	result = acos(arg1);
	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dasin			- returns the arcsin of arg1 (radians)
 */
Datum
dasin(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* Per the POSIX spec, return NaN if the input is NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/*
	 * The principal branch of the inverse sine function maps values in the
	 * range [-1, 1] to values in the range [-Pi/2, Pi/2], so we should reject
	 * any inputs outside that range and the result will always be finite.
	 */
	if (arg1 < -1.0 || arg1 > 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	result = asin(arg1);
	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		datan			- returns the arctan of arg1 (radians)
 */
Datum
datan(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* Per the POSIX spec, return NaN if the input is NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/*
	 * The principal branch of the inverse tangent function maps all inputs to
	 * values in the range [-Pi/2, Pi/2], so the result should always be
	 * finite, even if the input is infinite.
	 */
	result = atan(arg1);
	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		atan2			- returns the arctan of arg1/arg2 (radians)
 */
Datum
datan2(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	/* Per the POSIX spec, return NaN if either input is NaN */
	if (isnan(arg1) || isnan(arg2))
		PG_RETURN_FLOAT8(get_float8_nan());

	/*
	 * atan2 maps all inputs to values in the range [-Pi, Pi], so the result
	 * should always be finite, even if the inputs are infinite.
	 */
	result = atan2(arg1, arg2);
	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dcos			- returns the cosine of arg1 (radians)
 */
Datum
dcos(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* Per the POSIX spec, return NaN if the input is NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/*
	 * cos() is periodic and so theoretically can work for all finite inputs,
	 * but some implementations may choose to throw error if the input is so
	 * large that there are no significant digits in the result.  So we should
	 * check for errors.  POSIX allows an error to be reported either via
	 * errno or via fetestexcept(), but currently we only support checking
	 * errno.  (fetestexcept() is rumored to report underflow unreasonably
	 * early on some platforms, so it's not clear that believing it would be a
	 * net improvement anyway.)
	 *
	 * For infinite inputs, POSIX specifies that the trigonometric functions
	 * should return a domain error; but we won't notice that unless the
	 * platform reports via errno, so also explicitly test for infinite
	 * inputs.
	 */
	errno = 0;
	result = cos(arg1);
	if (errno != 0 || isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));
	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dcot			- returns the cotangent of arg1 (radians)
 */
Datum
dcot(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* Per the POSIX spec, return NaN if the input is NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/* Be sure to throw an error if the input is infinite --- see dcos() */
	errno = 0;
	result = tan(arg1);
	if (errno != 0 || isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	result = 1.0 / result;
	/* Not checking for overflow because cot(0) == Inf */

	PG_RETURN_FLOAT8(result);
}


/*
 *		dsin			- returns the sine of arg1 (radians)
 */
Datum
dsin(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* Per the POSIX spec, return NaN if the input is NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/* Be sure to throw an error if the input is infinite --- see dcos() */
	errno = 0;
	result = sin(arg1);
	if (errno != 0 || isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));
	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dtan			- returns the tangent of arg1 (radians)
 */
Datum
dtan(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* Per the POSIX spec, return NaN if the input is NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/* Be sure to throw an error if the input is infinite --- see dcos() */
	errno = 0;
	result = tan(arg1);
	if (errno != 0 || isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));
	/* Not checking for overflow because tan(pi/2) == Inf */

	PG_RETURN_FLOAT8(result);
}


/* ========== DEGREE-BASED TRIGONOMETRIC FUNCTIONS ========== */


/*
 * Initialize the cached constants declared at the head of this file
 * (sin_30 etc).  The fact that we need those at all, let alone need this
 * Rube-Goldberg-worthy method of initializing them, is because there are
 * compilers out there that will precompute expressions such as sin(constant)
 * using a sin() function different from what will be used at runtime.  If we
 * want exact results, we must ensure that none of the scaling constants used
 * in the degree-based trig functions are computed that way.  To do so, we
 * compute them from the variables degree_c_thirty etc, which are also really
 * constants, but the compiler cannot assume that.
 *
 * Other hazards we are trying to forestall with this kluge include the
 * possibility that compilers will rearrange the expressions, or compute
 * some intermediate results in registers wider than a standard double.
 *
 * In the places where we use these constants, the typical pattern is like
 *		volatile float8 sin_x = sin(x * RADIANS_PER_DEGREE);
 *		return (sin_x / sin_30);
 * where we hope to get a value of exactly 1.0 from the division when x = 30.
 * The volatile temporary variable is needed on machines with wide float
 * registers, to ensure that the result of sin(x) is rounded to double width
 * the same as the value of sin_30 has been.  Experimentation with gcc shows
 * that marking the temp variable volatile is necessary to make the store and
 * reload actually happen; hopefully the same trick works for other compilers.
 * (gcc's documentation suggests using the -ffloat-store compiler switch to
 * ensure this, but that is compiler-specific and it also pessimizes code in
 * many places where we don't care about this.)
 */
static void
init_degree_constants(void)
{
	sin_30 = sin(degree_c_thirty * RADIANS_PER_DEGREE);
	one_minus_cos_60 = 1.0 - cos(degree_c_sixty * RADIANS_PER_DEGREE);
	asin_0_5 = asin(degree_c_one_half);
	acos_0_5 = acos(degree_c_one_half);
	atan_1_0 = atan(degree_c_one);
	tan_45 = sind_q1(degree_c_forty_five) / cosd_q1(degree_c_forty_five);
	cot_45 = cosd_q1(degree_c_forty_five) / sind_q1(degree_c_forty_five);
	degree_consts_set = true;
}

#define INIT_DEGREE_CONSTANTS() \
do { \
	if (!degree_consts_set) \
		init_degree_constants(); \
} while(0)


/*
 *		asind_q1		- returns the inverse sine of x in degrees, for x in
 *						  the range [0, 1].  The result is an angle in the
 *						  first quadrant --- [0, 90] degrees.
 *
 *						  For the 3 special case inputs (0, 0.5 and 1), this
 *						  function will return exact values (0, 30 and 90
 *						  degrees respectively).
 */
static double
asind_q1(double x)
{
	/*
	 * Stitch together inverse sine and cosine functions for the ranges [0,
	 * 0.5] and (0.5, 1].  Each expression below is guaranteed to return
	 * exactly 30 for x=0.5, so the result is a continuous monotonic function
	 * over the full range.
	 */
	if (x <= 0.5)
	{
		volatile float8 asin_x = asin(x);

		return (asin_x / asin_0_5) * 30.0;
	}
	else
	{
		volatile float8 acos_x = acos(x);

		return 90.0 - (acos_x / acos_0_5) * 60.0;
	}
}


/*
 *		acosd_q1		- returns the inverse cosine of x in degrees, for x in
 *						  the range [0, 1].  The result is an angle in the
 *						  first quadrant --- [0, 90] degrees.
 *
 *						  For the 3 special case inputs (0, 0.5 and 1), this
 *						  function will return exact values (0, 60 and 90
 *						  degrees respectively).
 */
static double
acosd_q1(double x)
{
	/*
	 * Stitch together inverse sine and cosine functions for the ranges [0,
	 * 0.5] and (0.5, 1].  Each expression below is guaranteed to return
	 * exactly 60 for x=0.5, so the result is a continuous monotonic function
	 * over the full range.
	 */
	if (x <= 0.5)
	{
		volatile float8 asin_x = asin(x);

		return 90.0 - (asin_x / asin_0_5) * 30.0;
	}
	else
	{
		volatile float8 acos_x = acos(x);

		return (acos_x / acos_0_5) * 60.0;
	}
}


/*
 *		dacosd			- returns the arccos of arg1 (degrees)
 */
Datum
dacosd(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* Per the POSIX spec, return NaN if the input is NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	INIT_DEGREE_CONSTANTS();

	/*
	 * The principal branch of the inverse cosine function maps values in the
	 * range [-1, 1] to values in the range [0, 180], so we should reject any
	 * inputs outside that range and the result will always be finite.
	 */
	if (arg1 < -1.0 || arg1 > 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	if (arg1 >= 0.0)
		result = acosd_q1(arg1);
	else
		result = 90.0 + asind_q1(-arg1);

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dasind			- returns the arcsin of arg1 (degrees)
 */
Datum
dasind(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* Per the POSIX spec, return NaN if the input is NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	INIT_DEGREE_CONSTANTS();

	/*
	 * The principal branch of the inverse sine function maps values in the
	 * range [-1, 1] to values in the range [-90, 90], so we should reject any
	 * inputs outside that range and the result will always be finite.
	 */
	if (arg1 < -1.0 || arg1 > 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	if (arg1 >= 0.0)
		result = asind_q1(arg1);
	else
		result = -asind_q1(-arg1);

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		datand			- returns the arctan of arg1 (degrees)
 */
Datum
datand(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;
	volatile float8 atan_arg1;

	/* Per the POSIX spec, return NaN if the input is NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	INIT_DEGREE_CONSTANTS();

	/*
	 * The principal branch of the inverse tangent function maps all inputs to
	 * values in the range [-90, 90], so the result should always be finite,
	 * even if the input is infinite.  Additionally, we take care to ensure
	 * than when arg1 is 1, the result is exactly 45.
	 */
	atan_arg1 = atan(arg1);
	result = (atan_arg1 / atan_1_0) * 45.0;

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		atan2d			- returns the arctan of arg1/arg2 (degrees)
 */
Datum
datan2d(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;
	volatile float8 atan2_arg1_arg2;

	/* Per the POSIX spec, return NaN if either input is NaN */
	if (isnan(arg1) || isnan(arg2))
		PG_RETURN_FLOAT8(get_float8_nan());

	INIT_DEGREE_CONSTANTS();

	/*
	 * atan2d maps all inputs to values in the range [-180, 180], so the
	 * result should always be finite, even if the inputs are infinite.
	 *
	 * Note: this coding assumes that atan(1.0) is a suitable scaling constant
	 * to get an exact result from atan2().  This might well fail on us at
	 * some point, requiring us to decide exactly what inputs we think we're
	 * going to guarantee an exact result for.
	 */
	atan2_arg1_arg2 = atan2(arg1, arg2);
	result = (atan2_arg1_arg2 / atan_1_0) * 45.0;

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		sind_0_to_30	- returns the sine of an angle that lies between 0 and
 *						  30 degrees.  This will return exactly 0 when x is 0,
 *						  and exactly 0.5 when x is 30 degrees.
 */
static double
sind_0_to_30(double x)
{
	volatile float8 sin_x = sin(x * RADIANS_PER_DEGREE);

	return (sin_x / sin_30) / 2.0;
}


/*
 *		cosd_0_to_60	- returns the cosine of an angle that lies between 0
 *						  and 60 degrees.  This will return exactly 1 when x
 *						  is 0, and exactly 0.5 when x is 60 degrees.
 */
static double
cosd_0_to_60(double x)
{
	volatile float8 one_minus_cos_x = 1.0 - cos(x * RADIANS_PER_DEGREE);

	return 1.0 - (one_minus_cos_x / one_minus_cos_60) / 2.0;
}


/*
 *		sind_q1			- returns the sine of an angle in the first quadrant
 *						  (0 to 90 degrees).
 */
static double
sind_q1(double x)
{
	/*
	 * Stitch together the sine and cosine functions for the ranges [0, 30]
	 * and (30, 90].  These guarantee to return exact answers at their
	 * endpoints, so the overall result is a continuous monotonic function
	 * that gives exact results when x = 0, 30 and 90 degrees.
	 */
	if (x <= 30.0)
		return sind_0_to_30(x);
	else
		return cosd_0_to_60(90.0 - x);
}


/*
 *		cosd_q1			- returns the cosine of an angle in the first quadrant
 *						  (0 to 90 degrees).
 */
static double
cosd_q1(double x)
{
	/*
	 * Stitch together the sine and cosine functions for the ranges [0, 60]
	 * and (60, 90].  These guarantee to return exact answers at their
	 * endpoints, so the overall result is a continuous monotonic function
	 * that gives exact results when x = 0, 60 and 90 degrees.
	 */
	if (x <= 60.0)
		return cosd_0_to_60(x);
	else
		return sind_0_to_30(90.0 - x);
}


/*
 *		dcosd			- returns the cosine of arg1 (degrees)
 */
Datum
dcosd(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;
	int			sign = 1;

	/*
	 * Per the POSIX spec, return NaN if the input is NaN and throw an error
	 * if the input is infinite.
	 */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	if (isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	INIT_DEGREE_CONSTANTS();

	/* Reduce the range of the input to [0,90] degrees */
	arg1 = fmod(arg1, 360.0);

	if (arg1 < 0.0)
	{
		/* cosd(-x) = cosd(x) */
		arg1 = -arg1;
	}

	if (arg1 > 180.0)
	{
		/* cosd(360-x) = cosd(x) */
		arg1 = 360.0 - arg1;
	}

	if (arg1 > 90.0)
	{
		/* cosd(180-x) = -cosd(x) */
		arg1 = 180.0 - arg1;
		sign = -sign;
	}

	result = sign * cosd_q1(arg1);

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dcotd			- returns the cotangent of arg1 (degrees)
 */
Datum
dcotd(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;
	volatile float8 cot_arg1;
	int			sign = 1;

	/*
	 * Per the POSIX spec, return NaN if the input is NaN and throw an error
	 * if the input is infinite.
	 */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	if (isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	INIT_DEGREE_CONSTANTS();

	/* Reduce the range of the input to [0,90] degrees */
	arg1 = fmod(arg1, 360.0);

	if (arg1 < 0.0)
	{
		/* cotd(-x) = -cotd(x) */
		arg1 = -arg1;
		sign = -sign;
	}

	if (arg1 > 180.0)
	{
		/* cotd(360-x) = -cotd(x) */
		arg1 = 360.0 - arg1;
		sign = -sign;
	}

	if (arg1 > 90.0)
	{
		/* cotd(180-x) = -cotd(x) */
		arg1 = 180.0 - arg1;
		sign = -sign;
	}

	cot_arg1 = cosd_q1(arg1) / sind_q1(arg1);
	result = sign * (cot_arg1 / cot_45);

	/*
	 * On some machines we get cotd(270) = minus zero, but this isn't always
	 * true.  For portability, and because the user constituency for this
	 * function probably doesn't want minus zero, force it to plain zero.
	 */
	if (result == 0.0)
		result = 0.0;

	/* Not checking for overflow because cotd(0) == Inf */

	PG_RETURN_FLOAT8(result);
}


/*
 *		dsind			- returns the sine of arg1 (degrees)
 */
Datum
dsind(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;
	int			sign = 1;

	/*
	 * Per the POSIX spec, return NaN if the input is NaN and throw an error
	 * if the input is infinite.
	 */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	if (isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	INIT_DEGREE_CONSTANTS();

	/* Reduce the range of the input to [0,90] degrees */
	arg1 = fmod(arg1, 360.0);

	if (arg1 < 0.0)
	{
		/* sind(-x) = -sind(x) */
		arg1 = -arg1;
		sign = -sign;
	}

	if (arg1 > 180.0)
	{
		/* sind(360-x) = -sind(x) */
		arg1 = 360.0 - arg1;
		sign = -sign;
	}

	if (arg1 > 90.0)
	{
		/* sind(180-x) = sind(x) */
		arg1 = 180.0 - arg1;
	}

	result = sign * sind_q1(arg1);

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dtand			- returns the tangent of arg1 (degrees)
 */
Datum
dtand(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;
	volatile float8 tan_arg1;
	int			sign = 1;

	/*
	 * Per the POSIX spec, return NaN if the input is NaN and throw an error
	 * if the input is infinite.
	 */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	if (isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	INIT_DEGREE_CONSTANTS();

	/* Reduce the range of the input to [0,90] degrees */
	arg1 = fmod(arg1, 360.0);

	if (arg1 < 0.0)
	{
		/* tand(-x) = -tand(x) */
		arg1 = -arg1;
		sign = -sign;
	}

	if (arg1 > 180.0)
	{
		/* tand(360-x) = -tand(x) */
		arg1 = 360.0 - arg1;
		sign = -sign;
	}

	if (arg1 > 90.0)
	{
		/* tand(180-x) = -tand(x) */
		arg1 = 180.0 - arg1;
		sign = -sign;
	}

	tan_arg1 = sind_q1(arg1) / cosd_q1(arg1);
	result = sign * (tan_arg1 / tan_45);

	/*
	 * On some machines we get tand(180) = minus zero, but this isn't always
	 * true.  For portability, and because the user constituency for this
	 * function probably doesn't want minus zero, force it to plain zero.
	 */
	if (result == 0.0)
		result = 0.0;

	/* Not checking for overflow because tand(90) == Inf */

	PG_RETURN_FLOAT8(result);
}


/*
 *		degrees		- returns degrees converted from radians
 */
Datum
degrees(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(float8_div(arg1, RADIANS_PER_DEGREE));
}


/*
 *		dpi				- returns the constant PI
 */
Datum
dpi(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(M_PI);
}


/*
 *		radians		- returns radians converted from degrees
 */
Datum
radians(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(float8_mul(arg1, RADIANS_PER_DEGREE));
}


/* ========== HYPERBOLIC FUNCTIONS ========== */


/*
 *		dsinh			- returns the hyperbolic sine of arg1
 */
Datum
dsinh(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	errno = 0;
	result = sinh(arg1);

	/*
	 * if an ERANGE error occurs, it means there is an overflow.  For sinh,
	 * the result should be either -infinity or infinity, depending on the
	 * sign of arg1.
	 */
	if (errno == ERANGE)
	{
		if (arg1 < 0)
			result = -get_float8_infinity();
		else
			result = get_float8_infinity();
	}

	PG_RETURN_FLOAT8(result);
}


/*
 *		dcosh			- returns the hyperbolic cosine of arg1
 */
Datum
dcosh(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	errno = 0;
	result = cosh(arg1);

	/*
	 * if an ERANGE error occurs, it means there is an overflow.  As cosh is
	 * always positive, it always means the result is positive infinity.
	 */
	if (errno == ERANGE)
		result = get_float8_infinity();

	if (unlikely(result == 0.0))
		float_underflow_error();

	PG_RETURN_FLOAT8(result);
}

/*
 *		dtanh			- returns the hyperbolic tangent of arg1
 */
Datum
dtanh(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * For tanh, we don't need an errno check because it never overflows.
	 */
	result = tanh(arg1);

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}

/*
 *		dasinh			- returns the inverse hyperbolic sine of arg1
 */
Datum
dasinh(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * For asinh, we don't need an errno check because it never overflows.
	 */
	result = asinh(arg1);

	PG_RETURN_FLOAT8(result);
}

/*
 *		dacosh			- returns the inverse hyperbolic cosine of arg1
 */
Datum
dacosh(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * acosh is only defined for inputs >= 1.0.  By checking this ourselves,
	 * we need not worry about checking for an EDOM error, which is a good
	 * thing because some implementations will report that for NaN. Otherwise,
	 * no error is possible.
	 */
	if (arg1 < 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	result = acosh(arg1);

	PG_RETURN_FLOAT8(result);
}

/*
 *		datanh			- returns the inverse hyperbolic tangent of arg1
 */
Datum
datanh(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * atanh is only defined for inputs between -1 and 1.  By checking this
	 * ourselves, we need not worry about checking for an EDOM error, which is
	 * a good thing because some implementations will report that for NaN.
	 */
	if (arg1 < -1.0 || arg1 > 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	/*
	 * Also handle the infinity cases ourselves; this is helpful because old
	 * glibc versions may produce the wrong errno for this.  All other inputs
	 * cannot produce an error.
	 */
	if (arg1 == -1.0)
		result = -get_float8_infinity();
	else if (arg1 == 1.0)
		result = get_float8_infinity();
	else
		result = atanh(arg1);

	PG_RETURN_FLOAT8(result);
}


/*
 *		drandom		- returns a random number
 */
Datum
drandom(PG_FUNCTION_ARGS)
{
	float8		result;

	/* Initialize random seed, if not done yet in this process */
	if (unlikely(!drandom_seed_set))
	{
		/*
		 * If possible, initialize the seed using high-quality random bits.
		 * Should that fail for some reason, we fall back on a lower-quality
		 * seed based on current time and PID.
		 */
		if (!pg_strong_random(drandom_seed, sizeof(drandom_seed)))
		{
			TimestampTz now = GetCurrentTimestamp();
			uint64		iseed;

			/* Mix the PID with the most predictable bits of the timestamp */
			iseed = (uint64) now ^ ((uint64) MyProcPid << 32);
			drandom_seed[0] = (unsigned short) iseed;
			drandom_seed[1] = (unsigned short) (iseed >> 16);
			drandom_seed[2] = (unsigned short) (iseed >> 32);
		}
		drandom_seed_set = true;
	}

	/* pg_erand48 produces desired result range [0.0 - 1.0) */
	result = pg_erand48(drandom_seed);

	PG_RETURN_FLOAT8(result);
}


/*
 *		setseed		- set seed for the random number generator
 */
Datum
setseed(PG_FUNCTION_ARGS)
{
	float8		seed = PG_GETARG_FLOAT8(0);
	uint64		iseed;

	if (seed < -1 || seed > 1 || isnan(seed))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("setseed parameter %g is out of allowed range [-1,1]",
						seed)));

	/* Use sign bit + 47 fractional bits to fill drandom_seed[] */
	iseed = (int64) (seed * (float8) UINT64CONST(0x7FFFFFFFFFFF));
	drandom_seed[0] = (unsigned short) iseed;
	drandom_seed[1] = (unsigned short) (iseed >> 16);
	drandom_seed[2] = (unsigned short) (iseed >> 32);
	drandom_seed_set = true;

	PG_RETURN_VOID();
}



/*
 *		=========================
 *		FLOAT AGGREGATE OPERATORS
 *		=========================
 *
 *		float8_accum		- accumulate for AVG(), variance aggregates, etc.
 *		float4_accum		- same, but input data is float4
 *		float8_avg			- produce final result for float AVG()
 *		float8_var_samp		- produce final result for float VAR_SAMP()
 *		float8_var_pop		- produce final result for float VAR_POP()
 *		float8_stddev_samp	- produce final result for float STDDEV_SAMP()
 *		float8_stddev_pop	- produce final result for float STDDEV_POP()
 *
 * The naive schoolbook implementation of these aggregates works by
 * accumulating sum(X) and sum(X^2).  However, this approach suffers from
 * large rounding errors in the final computation of quantities like the
 * population variance (N*sum(X^2) - sum(X)^2) / N^2, since each of the
 * intermediate terms is potentially very large, while the difference is often
 * quite small.
 *
 * Instead we use the Youngs-Cramer algorithm [1] which works by accumulating
 * Sx=sum(X) and Sxx=sum((X-Sx/N)^2), using a numerically stable algorithm to
 * incrementally update those quantities.  The final computations of each of
 * the aggregate values is then trivial and gives more accurate results (for
 * example, the population variance is just Sxx/N).  This algorithm is also
 * fairly easy to generalize to allow parallel execution without loss of
 * precision (see, for example, [2]).  For more details, and a comparison of
 * this with other algorithms, see [3].
 *
 * The transition datatype for all these aggregates is a 3-element array
 * of float8, holding the values N, Sx, Sxx in that order.
 *
 * Note that we represent N as a float to avoid having to build a special
 * datatype.  Given a reasonable floating-point implementation, there should
 * be no accuracy loss unless N exceeds 2 ^ 52 or so (by which time the
 * user will have doubtless lost interest anyway...)
 *
 * [1] Some Results Relevant to Choice of Sum and Sum-of-Product Algorithms,
 * E. A. Youngs and E. M. Cramer, Technometrics Vol 13, No 3, August 1971.
 *
 * [2] Updating Formulae and a Pairwise Algorithm for Computing Sample
 * Variances, T. F. Chan, G. H. Golub & R. J. LeVeque, COMPSTAT 1982.
 *
 * [3] Numerically Stable Parallel Computation of (Co-)Variance, Erich
 * Schubert and Michael Gertz, Proceedings of the 30th International
 * Conference on Scientific and Statistical Database Management, 2018.
 */

static float8 *
check_float8_array(ArrayType *transarray, const char *caller, int n)
{
	/*
	 * We expect the input to be an N-element float array; verify that. We
	 * don't need to use deconstruct_array() since the array data is just
	 * going to look like a C array of N float8 values.
	 */
	if (ARR_NDIM(transarray) != 1 ||
		ARR_DIMS(transarray)[0] != n ||
		ARR_HASNULL(transarray) ||
		ARR_ELEMTYPE(transarray) != FLOAT8OID)
		elog(ERROR, "%s: expected %d-element float8 array", caller, n);
	return (float8 *) ARR_DATA_PTR(transarray);
}

/*
 * float8_combine
 *
 * An aggregate combine function used to combine two 3 fields
 * aggregate transition data into a single transition data.
 * This function is used only in two stage aggregation and
 * shouldn't be called outside aggregate context.
 */
Datum
float8_combine(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *transarray2 = PG_GETARG_ARRAYTYPE_P(1);
	float8	   *transvalues1;
	float8	   *transvalues2;
	float8		N1,
				Sx1,
				Sxx1,
				N2,
				Sx2,
				Sxx2,
				tmp,
				N,
				Sx,
				Sxx;

	transvalues1 = check_float8_array(transarray1, "float8_combine", 3);
	transvalues2 = check_float8_array(transarray2, "float8_combine", 3);

	N1 = transvalues1[0];
	Sx1 = transvalues1[1];
	Sxx1 = transvalues1[2];

	N2 = transvalues2[0];
	Sx2 = transvalues2[1];
	Sxx2 = transvalues2[2];

	/*--------------------
	 * The transition values combine using a generalization of the
	 * Youngs-Cramer algorithm as follows:
	 *
	 *	N = N1 + N2
	 *	Sx = Sx1 + Sx2
	 *	Sxx = Sxx1 + Sxx2 + N1 * N2 * (Sx1/N1 - Sx2/N2)^2 / N;
	 *
	 * It's worth handling the special cases N1 = 0 and N2 = 0 separately
	 * since those cases are trivial, and we then don't need to worry about
	 * division-by-zero errors in the general case.
	 *--------------------
	 */
	if (N1 == 0.0)
	{
		N = N2;
		Sx = Sx2;
		Sxx = Sxx2;
	}
	else if (N2 == 0.0)
	{
		N = N1;
		Sx = Sx1;
		Sxx = Sxx1;
	}
	else
	{
		N = N1 + N2;
		Sx = float8_pl(Sx1, Sx2);
		tmp = Sx1 / N1 - Sx2 / N2;
		Sxx = Sxx1 + Sxx2 + N1 * N2 * tmp * tmp / N;
		if (unlikely(isinf(Sxx)) && !isinf(Sxx1) && !isinf(Sxx2))
			float_overflow_error();
	}

	/*
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we construct a
	 * new array with the updated transition data and return it.
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues1[0] = N;
		transvalues1[1] = Sx;
		transvalues1[2] = Sxx;

		PG_RETURN_ARRAYTYPE_P(transarray1);
	}
	else
	{
		Datum		transdatums[3];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(Sx);
		transdatums[2] = Float8GetDatumFast(Sxx);

		result = construct_array(transdatums, 3,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

		PG_RETURN_ARRAYTYPE_P(result);
	}
}

Datum
float8_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8		newval = PG_GETARG_FLOAT8(1);
	float8	   *transvalues;
	float8		N,
				Sx,
				Sxx,
				tmp;

	transvalues = check_float8_array(transarray, "float8_accum", 3);
	N = transvalues[0];
	Sx = transvalues[1];
	Sxx = transvalues[2];

	/*
	 * Use the Youngs-Cramer algorithm to incorporate the new value into the
	 * transition values.
	 */
	N += 1.0;
	Sx += newval;
	if (transvalues[0] > 0.0)
	{
		tmp = newval * N - Sx;
		Sxx += tmp * tmp / (N * transvalues[0]);

		/*
		 * Overflow check.  We only report an overflow error when finite
		 * inputs lead to infinite results.  Note also that Sxx should be NaN
		 * if any of the inputs are infinite, so we intentionally prevent Sxx
		 * from becoming infinite.
		 */
		if (isinf(Sx) || isinf(Sxx))
		{
			if (!isinf(transvalues[1]) && !isinf(newval))
				float_overflow_error();

			Sxx = get_float8_nan();
		}
	}
	else
	{
		/*
		 * At the first input, we normally can leave Sxx as 0.  However, if
		 * the first input is Inf or NaN, we'd better force Sxx to NaN;
		 * otherwise we will falsely report variance zero when there are no
		 * more inputs.
		 */
		if (isnan(newval) || isinf(newval))
			Sxx = get_float8_nan();
	}

	/*
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we construct a
	 * new array with the updated transition data and return it.
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues[0] = N;
		transvalues[1] = Sx;
		transvalues[2] = Sxx;

		PG_RETURN_ARRAYTYPE_P(transarray);
	}
	else
	{
		Datum		transdatums[3];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(Sx);
		transdatums[2] = Float8GetDatumFast(Sxx);

		result = construct_array(transdatums, 3,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

		PG_RETURN_ARRAYTYPE_P(result);
	}
}

Datum
float4_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);

	/* do computations as float8 */
	float8		newval = PG_GETARG_FLOAT4(1);
	float8	   *transvalues;
	float8		N,
				Sx,
				Sxx,
				tmp;

	transvalues = check_float8_array(transarray, "float4_accum", 3);
	N = transvalues[0];
	Sx = transvalues[1];
	Sxx = transvalues[2];

	/*
	 * Use the Youngs-Cramer algorithm to incorporate the new value into the
	 * transition values.
	 */
	N += 1.0;
	Sx += newval;
	if (transvalues[0] > 0.0)
	{
		tmp = newval * N - Sx;
		Sxx += tmp * tmp / (N * transvalues[0]);

		/*
		 * Overflow check.  We only report an overflow error when finite
		 * inputs lead to infinite results.  Note also that Sxx should be NaN
		 * if any of the inputs are infinite, so we intentionally prevent Sxx
		 * from becoming infinite.
		 */
		if (isinf(Sx) || isinf(Sxx))
		{
			if (!isinf(transvalues[1]) && !isinf(newval))
				float_overflow_error();

			Sxx = get_float8_nan();
		}
	}
	else
	{
		/*
		 * At the first input, we normally can leave Sxx as 0.  However, if
		 * the first input is Inf or NaN, we'd better force Sxx to NaN;
		 * otherwise we will falsely report variance zero when there are no
		 * more inputs.
		 */
		if (isnan(newval) || isinf(newval))
			Sxx = get_float8_nan();
	}

	/*
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we construct a
	 * new array with the updated transition data and return it.
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues[0] = N;
		transvalues[1] = Sx;
		transvalues[2] = Sxx;

		PG_RETURN_ARRAYTYPE_P(transarray);
	}
	else
	{
		Datum		transdatums[3];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(Sx);
		transdatums[2] = Float8GetDatumFast(Sxx);

		result = construct_array(transdatums, 3,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

		PG_RETURN_ARRAYTYPE_P(result);
	}
}

Datum
float8_avg(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sx;

	transvalues = check_float8_array(transarray, "float8_avg", 3);
	N = transvalues[0];
	Sx = transvalues[1];
	/* ignore Sxx */

	/* SQL defines AVG of no values to be NULL */
	if (N == 0.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sx / N);
}

Datum
float8_var_pop(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx;

	transvalues = check_float8_array(transarray, "float8_var_pop", 3);
	N = transvalues[0];
	/* ignore Sx */
	Sxx = transvalues[2];

	/* Population variance is undefined when N is 0, so return NULL */
	if (N == 0.0)
		PG_RETURN_NULL();

	/* Note that Sxx is guaranteed to be non-negative */

	PG_RETURN_FLOAT8(Sxx / N);
}

Datum
float8_var_samp(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx;

	transvalues = check_float8_array(transarray, "float8_var_samp", 3);
	N = transvalues[0];
	/* ignore Sx */
	Sxx = transvalues[2];

	/* Sample variance is undefined when N is 0 or 1, so return NULL */
	if (N <= 1.0)
		PG_RETURN_NULL();

	/* Note that Sxx is guaranteed to be non-negative */

	PG_RETURN_FLOAT8(Sxx / (N - 1.0));
}

Datum
float8_stddev_pop(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx;

	transvalues = check_float8_array(transarray, "float8_stddev_pop", 3);
	N = transvalues[0];
	/* ignore Sx */
	Sxx = transvalues[2];

	/* Population stddev is undefined when N is 0, so return NULL */
	if (N == 0.0)
		PG_RETURN_NULL();

	/* Note that Sxx is guaranteed to be non-negative */

	PG_RETURN_FLOAT8(sqrt(Sxx / N));
}

Datum
float8_stddev_samp(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx;

	transvalues = check_float8_array(transarray, "float8_stddev_samp", 3);
	N = transvalues[0];
	/* ignore Sx */
	Sxx = transvalues[2];

	/* Sample stddev is undefined when N is 0 or 1, so return NULL */
	if (N <= 1.0)
		PG_RETURN_NULL();

	/* Note that Sxx is guaranteed to be non-negative */

	PG_RETURN_FLOAT8(sqrt(Sxx / (N - 1.0)));
}

/*
 *		=========================
 *		SQL2003 BINARY AGGREGATES
 *		=========================
 *
 * As with the preceding aggregates, we use the Youngs-Cramer algorithm to
 * reduce rounding errors in the aggregate final functions.
 *
 * The transition datatype for all these aggregates is a 6-element array of
 * float8, holding the values N, Sx=sum(X), Sxx=sum((X-Sx/N)^2), Sy=sum(Y),
 * Syy=sum((Y-Sy/N)^2), Sxy=sum((X-Sx/N)*(Y-Sy/N)) in that order.
 *
 * Note that Y is the first argument to all these aggregates!
 *
 * It might seem attractive to optimize this by having multiple accumulator
 * functions that only calculate the sums actually needed.  But on most
 * modern machines, a couple of extra floating-point multiplies will be
 * insignificant compared to the other per-tuple overhead, so I've chosen
 * to minimize code space instead.
 */

Datum
float8_regr_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8		newvalY = PG_GETARG_FLOAT8(1);
	float8		newvalX = PG_GETARG_FLOAT8(2);
	float8	   *transvalues;
	float8		N,
				Sx,
				Sxx,
				Sy,
				Syy,
				Sxy,
				tmpX,
				tmpY,
				scale;

	transvalues = check_float8_array(transarray, "float8_regr_accum", 6);
	N = transvalues[0];
	Sx = transvalues[1];
	Sxx = transvalues[2];
	Sy = transvalues[3];
	Syy = transvalues[4];
	Sxy = transvalues[5];

	/*
	 * Use the Youngs-Cramer algorithm to incorporate the new values into the
	 * transition values.
	 */
	N += 1.0;
	Sx += newvalX;
	Sy += newvalY;
	if (transvalues[0] > 0.0)
	{
		tmpX = newvalX * N - Sx;
		tmpY = newvalY * N - Sy;
		scale = 1.0 / (N * transvalues[0]);
		Sxx += tmpX * tmpX * scale;
		Syy += tmpY * tmpY * scale;
		Sxy += tmpX * tmpY * scale;

		/*
		 * Overflow check.  We only report an overflow error when finite
		 * inputs lead to infinite results.  Note also that Sxx, Syy and Sxy
		 * should be NaN if any of the relevant inputs are infinite, so we
		 * intentionally prevent them from becoming infinite.
		 */
		if (isinf(Sx) || isinf(Sxx) || isinf(Sy) || isinf(Syy) || isinf(Sxy))
		{
			if (((isinf(Sx) || isinf(Sxx)) &&
				 !isinf(transvalues[1]) && !isinf(newvalX)) ||
				((isinf(Sy) || isinf(Syy)) &&
				 !isinf(transvalues[3]) && !isinf(newvalY)) ||
				(isinf(Sxy) &&
				 !isinf(transvalues[1]) && !isinf(newvalX) &&
				 !isinf(transvalues[3]) && !isinf(newvalY)))
				float_overflow_error();

			if (isinf(Sxx))
				Sxx = get_float8_nan();
			if (isinf(Syy))
				Syy = get_float8_nan();
			if (isinf(Sxy))
				Sxy = get_float8_nan();
		}
	}
	else
	{
		/*
		 * At the first input, we normally can leave Sxx et al as 0.  However,
		 * if the first input is Inf or NaN, we'd better force the dependent
		 * sums to NaN; otherwise we will falsely report variance zero when
		 * there are no more inputs.
		 */
		if (isnan(newvalX) || isinf(newvalX))
			Sxx = Sxy = get_float8_nan();
		if (isnan(newvalY) || isinf(newvalY))
			Syy = Sxy = get_float8_nan();
	}

	/*
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we construct a
	 * new array with the updated transition data and return it.
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues[0] = N;
		transvalues[1] = Sx;
		transvalues[2] = Sxx;
		transvalues[3] = Sy;
		transvalues[4] = Syy;
		transvalues[5] = Sxy;

		PG_RETURN_ARRAYTYPE_P(transarray);
	}
	else
	{
		Datum		transdatums[6];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(Sx);
		transdatums[2] = Float8GetDatumFast(Sxx);
		transdatums[3] = Float8GetDatumFast(Sy);
		transdatums[4] = Float8GetDatumFast(Syy);
		transdatums[5] = Float8GetDatumFast(Sxy);

		result = construct_array(transdatums, 6,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

		PG_RETURN_ARRAYTYPE_P(result);
	}
}

/*
 * float8_regr_combine
 *
 * An aggregate combine function used to combine two 6 fields
 * aggregate transition data into a single transition data.
 * This function is used only in two stage aggregation and
 * shouldn't be called outside aggregate context.
 */
Datum
float8_regr_combine(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *transarray2 = PG_GETARG_ARRAYTYPE_P(1);
	float8	   *transvalues1;
	float8	   *transvalues2;
	float8		N1,
				Sx1,
				Sxx1,
				Sy1,
				Syy1,
				Sxy1,
				N2,
				Sx2,
				Sxx2,
				Sy2,
				Syy2,
				Sxy2,
				tmp1,
				tmp2,
				N,
				Sx,
				Sxx,
				Sy,
				Syy,
				Sxy;

	transvalues1 = check_float8_array(transarray1, "float8_regr_combine", 6);
	transvalues2 = check_float8_array(transarray2, "float8_regr_combine", 6);

	N1 = transvalues1[0];
	Sx1 = transvalues1[1];
	Sxx1 = transvalues1[2];
	Sy1 = transvalues1[3];
	Syy1 = transvalues1[4];
	Sxy1 = transvalues1[5];

	N2 = transvalues2[0];
	Sx2 = transvalues2[1];
	Sxx2 = transvalues2[2];
	Sy2 = transvalues2[3];
	Syy2 = transvalues2[4];
	Sxy2 = transvalues2[5];

	/*--------------------
	 * The transition values combine using a generalization of the
	 * Youngs-Cramer algorithm as follows:
	 *
	 *	N = N1 + N2
	 *	Sx = Sx1 + Sx2
	 *	Sxx = Sxx1 + Sxx2 + N1 * N2 * (Sx1/N1 - Sx2/N2)^2 / N
	 *	Sy = Sy1 + Sy2
	 *	Syy = Syy1 + Syy2 + N1 * N2 * (Sy1/N1 - Sy2/N2)^2 / N
	 *	Sxy = Sxy1 + Sxy2 + N1 * N2 * (Sx1/N1 - Sx2/N2) * (Sy1/N1 - Sy2/N2) / N
	 *
	 * It's worth handling the special cases N1 = 0 and N2 = 0 separately
	 * since those cases are trivial, and we then don't need to worry about
	 * division-by-zero errors in the general case.
	 *--------------------
	 */
	if (N1 == 0.0)
	{
		N = N2;
		Sx = Sx2;
		Sxx = Sxx2;
		Sy = Sy2;
		Syy = Syy2;
		Sxy = Sxy2;
	}
	else if (N2 == 0.0)
	{
		N = N1;
		Sx = Sx1;
		Sxx = Sxx1;
		Sy = Sy1;
		Syy = Syy1;
		Sxy = Sxy1;
	}
	else
	{
		N = N1 + N2;
		Sx = float8_pl(Sx1, Sx2);
		tmp1 = Sx1 / N1 - Sx2 / N2;
		Sxx = Sxx1 + Sxx2 + N1 * N2 * tmp1 * tmp1 / N;
		if (unlikely(isinf(Sxx)) && !isinf(Sxx1) && !isinf(Sxx2))
			float_overflow_error();
		Sy = float8_pl(Sy1, Sy2);
		tmp2 = Sy1 / N1 - Sy2 / N2;
		Syy = Syy1 + Syy2 + N1 * N2 * tmp2 * tmp2 / N;
		if (unlikely(isinf(Syy)) && !isinf(Syy1) && !isinf(Syy2))
			float_overflow_error();
		Sxy = Sxy1 + Sxy2 + N1 * N2 * tmp1 * tmp2 / N;
		if (unlikely(isinf(Sxy)) && !isinf(Sxy1) && !isinf(Sxy2))
			float_overflow_error();
	}

	/*
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we construct a
	 * new array with the updated transition data and return it.
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues1[0] = N;
		transvalues1[1] = Sx;
		transvalues1[2] = Sxx;
		transvalues1[3] = Sy;
		transvalues1[4] = Syy;
		transvalues1[5] = Sxy;

		PG_RETURN_ARRAYTYPE_P(transarray1);
	}
	else
	{
		Datum		transdatums[6];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(Sx);
		transdatums[2] = Float8GetDatumFast(Sxx);
		transdatums[3] = Float8GetDatumFast(Sy);
		transdatums[4] = Float8GetDatumFast(Syy);
		transdatums[5] = Float8GetDatumFast(Sxy);

		result = construct_array(transdatums, 6,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

		PG_RETURN_ARRAYTYPE_P(result);
	}
}


Datum
float8_regr_sxx(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx;

	transvalues = check_float8_array(transarray, "float8_regr_sxx", 6);
	N = transvalues[0];
	Sxx = transvalues[2];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* Note that Sxx is guaranteed to be non-negative */

	PG_RETURN_FLOAT8(Sxx);
}

Datum
float8_regr_syy(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Syy;

	transvalues = check_float8_array(transarray, "float8_regr_syy", 6);
	N = transvalues[0];
	Syy = transvalues[4];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* Note that Syy is guaranteed to be non-negative */

	PG_RETURN_FLOAT8(Syy);
}

Datum
float8_regr_sxy(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_regr_sxy", 6);
	N = transvalues[0];
	Sxy = transvalues[5];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* A negative result is valid here */

	PG_RETURN_FLOAT8(Sxy);
}

Datum
float8_regr_avgx(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sx;

	transvalues = check_float8_array(transarray, "float8_regr_avgx", 6);
	N = transvalues[0];
	Sx = transvalues[1];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sx / N);
}

Datum
float8_regr_avgy(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sy;

	transvalues = check_float8_array(transarray, "float8_regr_avgy", 6);
	N = transvalues[0];
	Sy = transvalues[3];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sy / N);
}

Datum
float8_covar_pop(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_covar_pop", 6);
	N = transvalues[0];
	Sxy = transvalues[5];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sxy / N);
}

Datum
float8_covar_samp(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_covar_samp", 6);
	N = transvalues[0];
	Sxy = transvalues[5];

	/* if N is <= 1 we should return NULL */
	if (N < 2.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sxy / (N - 1.0));
}

Datum
float8_corr(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx,
				Syy,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_corr", 6);
	N = transvalues[0];
	Sxx = transvalues[2];
	Syy = transvalues[4];
	Sxy = transvalues[5];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* Note that Sxx and Syy are guaranteed to be non-negative */

	/* per spec, return NULL for horizontal and vertical lines */
	if (Sxx == 0 || Syy == 0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sxy / sqrt(Sxx * Syy));
}

Datum
float8_regr_r2(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx,
				Syy,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_regr_r2", 6);
	N = transvalues[0];
	Sxx = transvalues[2];
	Syy = transvalues[4];
	Sxy = transvalues[5];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* Note that Sxx and Syy are guaranteed to be non-negative */

	/* per spec, return NULL for a vertical line */
	if (Sxx == 0)
		PG_RETURN_NULL();

	/* per spec, return 1.0 for a horizontal line */
	if (Syy == 0)
		PG_RETURN_FLOAT8(1.0);

	PG_RETURN_FLOAT8((Sxy * Sxy) / (Sxx * Syy));
}

Datum
float8_regr_slope(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_regr_slope", 6);
	N = transvalues[0];
	Sxx = transvalues[2];
	Sxy = transvalues[5];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* Note that Sxx is guaranteed to be non-negative */

	/* per spec, return NULL for a vertical line */
	if (Sxx == 0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sxy / Sxx);
}

Datum
float8_regr_intercept(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sx,
				Sxx,
				Sy,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_regr_intercept", 6);
	N = transvalues[0];
	Sx = transvalues[1];
	Sxx = transvalues[2];
	Sy = transvalues[3];
	Sxy = transvalues[5];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* Note that Sxx is guaranteed to be non-negative */

	/* per spec, return NULL for a vertical line */
	if (Sxx == 0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8((Sy - Sx * Sxy / Sxx) / N);
}


/*
 *		====================================
 *		MIXED-PRECISION ARITHMETIC OPERATORS
 *		====================================
 */

/*
 *		float48pl		- returns arg1 + arg2
 *		float48mi		- returns arg1 - arg2
 *		float48mul		- returns arg1 * arg2
 *		float48div		- returns arg1 / arg2
 */
Datum
float48pl(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_pl((float8) arg1, arg2));
}

Datum
float48mi(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_mi((float8) arg1, arg2));
}

Datum
float48mul(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_mul((float8) arg1, arg2));
}

Datum
float48div(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_div((float8) arg1, arg2));
}

/*
 *		float84pl		- returns arg1 + arg2
 *		float84mi		- returns arg1 - arg2
 *		float84mul		- returns arg1 * arg2
 *		float84div		- returns arg1 / arg2
 */
Datum
float84pl(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT8(float8_pl(arg1, (float8) arg2));
}

Datum
float84mi(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT8(float8_mi(arg1, (float8) arg2));
}

Datum
float84mul(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT8(float8_mul(arg1, (float8) arg2));
}

Datum
float84div(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT8(float8_div(arg1, (float8) arg2));
}

/*
 *		====================
 *		COMPARISON OPERATORS
 *		====================
 */

/*
 *		float48{eq,ne,lt,le,gt,ge}		- float4/float8 comparison operations
 */
Datum
float48eq(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_eq((float8) arg1, arg2));
}

Datum
float48ne(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_ne((float8) arg1, arg2));
}

Datum
float48lt(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_lt((float8) arg1, arg2));
}

Datum
float48le(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_le((float8) arg1, arg2));
}

Datum
float48gt(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_gt((float8) arg1, arg2));
}

Datum
float48ge(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_ge((float8) arg1, arg2));
}

/*
 *		float84{eq,ne,lt,le,gt,ge}		- float8/float4 comparison operations
 */
Datum
float84eq(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_eq(arg1, (float8) arg2));
}

Datum
float84ne(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_ne(arg1, (float8) arg2));
}

Datum
float84lt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_lt(arg1, (float8) arg2));
}

Datum
float84le(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_le(arg1, (float8) arg2));
}

Datum
float84gt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_gt(arg1, (float8) arg2));
}

Datum
float84ge(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_ge(arg1, (float8) arg2));
}

/*
 * Implements the float8 version of the width_bucket() function
 * defined by SQL2003. See also width_bucket_numeric().
 *
 * 'bound1' and 'bound2' are the lower and upper bounds of the
 * histogram's range, respectively. 'count' is the number of buckets
 * in the histogram. width_bucket() returns an integer indicating the
 * bucket number that 'operand' belongs to in an equiwidth histogram
 * with the specified characteristics. An operand smaller than the
 * lower bound is assigned to bucket 0. An operand greater than the
 * upper bound is assigned to an additional bucket (with number
 * count+1). We don't allow "NaN" for any of the float8 inputs, and we
 * don't allow either of the histogram bounds to be +/- infinity.
 */
Datum
width_bucket_float8(PG_FUNCTION_ARGS)
{
	float8		operand = PG_GETARG_FLOAT8(0);
	float8		bound1 = PG_GETARG_FLOAT8(1);
	float8		bound2 = PG_GETARG_FLOAT8(2);
	int32		count = PG_GETARG_INT32(3);
	int32		result;

	if (count <= 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
				 errmsg("count must be greater than zero")));

	if (isnan(operand) || isnan(bound1) || isnan(bound2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
				 errmsg("operand, lower bound, and upper bound cannot be NaN")));

	/* Note that we allow "operand" to be infinite */
	if (isinf(bound1) || isinf(bound2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
				 errmsg("lower and upper bounds must be finite")));

	if (bound1 < bound2)
	{
		if (operand < bound1)
			result = 0;
		else if (operand >= bound2)
		{
			if (pg_add_s32_overflow(count, 1, &result))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("integer out of range")));
		}
		else
			result = ((float8) count * (operand - bound1) / (bound2 - bound1)) + 1;
	}
	else if (bound1 > bound2)
	{
		if (operand > bound1)
			result = 0;
		else if (operand <= bound2)
		{
			if (pg_add_s32_overflow(count, 1, &result))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("integer out of range")));
		}
		else
			result = ((float8) count * (bound1 - operand) / (bound1 - bound2)) + 1;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
				 errmsg("lower bound cannot equal upper bound")));
		result = 0;				/* keep the compiler quiet */
	}

	PG_RETURN_INT32(result);
}
