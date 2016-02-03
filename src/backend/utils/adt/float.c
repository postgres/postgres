/*-------------------------------------------------------------------------
 *
 * float.c
 *	  Functions for the built-in floating-point types.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/sortsupport.h"


#ifndef M_PI
/* from my RH5.2 gcc math.h file - thomas 2000-04-03 */
#define M_PI 3.14159265358979323846
#endif

/* Radians per degree, a.k.a. PI / 180 */
#define RADIANS_PER_DEGREE 0.0174532925199432957692

/* Visual C++ etc lacks NAN, and won't accept 0.0/0.0.  NAN definition from
 * http://msdn.microsoft.com/library/default.asp?url=/library/en-us/vclang/html/vclrfNotNumberNANItems.asp
 */
#if defined(WIN32) && !defined(NAN)
static const uint32 nan[2] = {0xffffffff, 0x7fffffff};

#define NAN (*(const double *) nan)
#endif

/* not sure what the following should be, but better to make it over-sufficient */
#define MAXFLOATWIDTH	64
#define MAXDOUBLEWIDTH	128

/*
 * check to see if a float4/8 val has underflowed or overflowed
 */
#define CHECKFLOATVAL(val, inf_is_valid, zero_is_valid)			\
do {															\
	if (isinf(val) && !(inf_is_valid))							\
		ereport(ERROR,											\
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),	\
		  errmsg("value out of range: overflow")));				\
																\
	if ((val) == 0.0 && !(zero_is_valid))						\
		ereport(ERROR,											\
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),	\
		 errmsg("value out of range: underflow")));				\
} while(0)


/* Configurable GUC parameter */
int			extra_float_digits = 0;		/* Added to DBL_DIG or FLT_DIG */

/* Cached constants for degree-based trig functions */
static bool degree_consts_set = false;
static float8 sin_30 = 0;
static float8 one_minus_cos_60 = 0;
static float8 asin_0_5 = 0;
static float8 acos_0_5 = 0;
static float8 atan_1_0 = 0;
static float8 tan_45 = 0;
static float8 cot_45 = 0;

/* Local function prototypes */
static int	float4_cmp_internal(float4 a, float4 b);
static int	float8_cmp_internal(float8 a, float8 b);
static double sind_q1(double x);
static double cosd_q1(double x);

/* This is INTENTIONALLY NOT STATIC.  Don't "fix" it. */
void init_degree_constants(float8 thirty, float8 forty_five, float8 sixty,
					  float8 one_half, float8 one);

#ifndef HAVE_CBRT
/*
 * Some machines (in particular, some versions of AIX) have an extern
 * declaration for cbrt() in <math.h> but fail to provide the actual
 * function, which causes configure to not set HAVE_CBRT.  Furthermore,
 * their compilers spit up at the mismatch between extern declaration
 * and static definition.  We work around that here by the expedient
 * of a #define to make the actual name of the static function different.
 */
#define cbrt my_cbrt
static double cbrt(double x);
#endif   /* HAVE_CBRT */


/*
 * Routines to provide reasonably platform-independent handling of
 * infinity and NaN.  We assume that isinf() and isnan() are available
 * and work per spec.  (On some platforms, we have to supply our own;
 * see src/port.)  However, generating an Infinity or NaN in the first
 * place is less well standardized; pre-C99 systems tend not to have C99's
 * INFINITY and NAN macros.  We centralize our workarounds for this here.
 */

double
get_float8_infinity(void)
{
#ifdef INFINITY
	/* C99 standard way */
	return (double) INFINITY;
#else

	/*
	 * On some platforms, HUGE_VAL is an infinity, elsewhere it's just the
	 * largest normal double.  We assume forcing an overflow will get us a
	 * true infinity.
	 */
	return (double) (HUGE_VAL * HUGE_VAL);
#endif
}

/*
* The funny placements of the two #pragmas is necessary because of a
* long lived bug in the Microsoft compilers.
* See http://support.microsoft.com/kb/120968/en-us for details
*/
#if (_MSC_VER >= 1800)
#pragma warning(disable:4756)
#endif
float
get_float4_infinity(void)
{
#ifdef INFINITY
	/* C99 standard way */
	return (float) INFINITY;
#else
#if (_MSC_VER >= 1800)
#pragma warning(default:4756)
#endif

	/*
	 * On some platforms, HUGE_VAL is an infinity, elsewhere it's just the
	 * largest normal double.  We assume forcing an overflow will get us a
	 * true infinity.
	 */
	return (float) (HUGE_VAL * HUGE_VAL);
#endif
}

double
get_float8_nan(void)
{
	/* (double) NAN doesn't work on some NetBSD/MIPS releases */
#if defined(NAN) && !(defined(__NetBSD__) && defined(__mips__))
	/* C99 standard way */
	return (double) NAN;
#else
	/* Assume we can get a NAN via zero divide */
	return (double) (0.0 / 0.0);
#endif
}

float
get_float4_nan(void)
{
#ifdef NAN
	/* C99 standard way */
	return (float) NAN;
#else
	/* Assume we can get a NAN via zero divide */
	return (float) (0.0 / 0.0);
#endif
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
 */
Datum
float4in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);
	char	   *orig_num;
	double		val;
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
				 errmsg("invalid input syntax for type real: \"%s\"",
						orig_num)));

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
			 */
			if (val == 0.0 || val >= HUGE_VAL || val <= -HUGE_VAL)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("\"%s\" is out of range for type real",
								orig_num)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type real: \"%s\"",
							orig_num)));
	}
#ifdef HAVE_BUGGY_SOLARIS_STRTOD
	else
	{
		/*
		 * Many versions of Solaris have a bug wherein strtod sets endptr to
		 * point one byte beyond the end of the string when given "inf" or
		 * "infinity".
		 */
		if (endptr != num && endptr[-1] == '\0')
			endptr--;
	}
#endif   /* HAVE_BUGGY_SOLARIS_STRTOD */

	/* skip trailing whitespace */
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;

	/* if there is any junk left at the end of the string, bail out */
	if (*endptr != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type real: \"%s\"",
						orig_num)));

	/*
	 * if we get here, we have a legal double, still need to check to see if
	 * it's a legal float4
	 */
	CHECKFLOATVAL((float4) val, isinf(val), val == 0);

	PG_RETURN_FLOAT4((float4) val);
}

/*
 *		float4out		- converts a float4 number to a string
 *						  using a standard output format
 */
Datum
float4out(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);
	char	   *ascii = (char *) palloc(MAXFLOATWIDTH + 1);

	if (isnan(num))
		PG_RETURN_CSTRING(strcpy(ascii, "NaN"));

	switch (is_infinite(num))
	{
		case 1:
			strcpy(ascii, "Infinity");
			break;
		case -1:
			strcpy(ascii, "-Infinity");
			break;
		default:
			{
				int			ndig = FLT_DIG + extra_float_digits;

				if (ndig < 1)
					ndig = 1;

				snprintf(ascii, MAXFLOATWIDTH + 1, "%.*g", ndig, num);
			}
	}

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
	char	   *orig_num;
	double		val;
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
			 errmsg("invalid input syntax for type double precision: \"%s\"",
					orig_num)));

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
			 */
			if (val == 0.0 || val >= HUGE_VAL || val <= -HUGE_VAL)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				   errmsg("\"%s\" is out of range for type double precision",
						  orig_num)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type double precision: \"%s\"",
					orig_num)));
	}
#ifdef HAVE_BUGGY_SOLARIS_STRTOD
	else
	{
		/*
		 * Many versions of Solaris have a bug wherein strtod sets endptr to
		 * point one byte beyond the end of the string when given "inf" or
		 * "infinity".
		 */
		if (endptr != num && endptr[-1] == '\0')
			endptr--;
	}
#endif   /* HAVE_BUGGY_SOLARIS_STRTOD */

	/* skip trailing whitespace */
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;

	/* if there is any junk left at the end of the string, bail out */
	if (*endptr != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type double precision: \"%s\"",
					orig_num)));

	CHECKFLOATVAL(val, true, true);

	PG_RETURN_FLOAT8(val);
}

/*
 *		float8out		- converts float8 number to a string
 *						  using a standard output format
 */
Datum
float8out(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);
	char	   *ascii = (char *) palloc(MAXDOUBLEWIDTH + 1);

	if (isnan(num))
		PG_RETURN_CSTRING(strcpy(ascii, "NaN"));

	switch (is_infinite(num))
	{
		case 1:
			strcpy(ascii, "Infinity");
			break;
		case -1:
			strcpy(ascii, "-Infinity");
			break;
		default:
			{
				int			ndig = DBL_DIG + extra_float_digits;

				if (ndig < 1)
					ndig = 1;

				snprintf(ascii, MAXDOUBLEWIDTH + 1, "%.*g", ndig, num);
			}
	}

	PG_RETURN_CSTRING(ascii);
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

	if (float4_cmp_internal(arg1, arg2) > 0)
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

	if (float4_cmp_internal(arg1, arg2) < 0)
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

	if (float8_cmp_internal(arg1, arg2) > 0)
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

	if (float8_cmp_internal(arg1, arg2) < 0)
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
	float4		result;

	result = arg1 + arg2;

	/*
	 * There isn't any way to check for underflow of addition/subtraction
	 * because numbers near the underflow value have already been rounded to
	 * the point where we can't detect that the two values were originally
	 * different, e.g. on x86, '1e-45'::float4 == '2e-45'::float4 ==
	 * 1.4013e-45.
	 */
	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), true);
	PG_RETURN_FLOAT4(result);
}

Datum
float4mi(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);
	float4		result;

	result = arg1 - arg2;
	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), true);
	PG_RETURN_FLOAT4(result);
}

Datum
float4mul(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);
	float4		result;

	result = arg1 * arg2;
	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2),
				  arg1 == 0 || arg2 == 0);
	PG_RETURN_FLOAT4(result);
}

Datum
float4div(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);
	float4		result;

	if (arg2 == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = arg1 / arg2;

	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), arg1 == 0);
	PG_RETURN_FLOAT4(result);
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
	float8		result;

	result = arg1 + arg2;

	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), true);
	PG_RETURN_FLOAT8(result);
}

Datum
float8mi(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	result = arg1 - arg2;

	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), true);
	PG_RETURN_FLOAT8(result);
}

Datum
float8mul(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	result = arg1 * arg2;

	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2),
				  arg1 == 0 || arg2 == 0);
	PG_RETURN_FLOAT8(result);
}

Datum
float8div(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	if (arg2 == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = arg1 / arg2;

	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), arg1 == 0);
	PG_RETURN_FLOAT8(result);
}


/*
 *		====================
 *		COMPARISON OPERATORS
 *		====================
 */

/*
 *		float4{eq,ne,lt,le,gt,ge}		- float4/float4 comparison operations
 */
static int
float4_cmp_internal(float4 a, float4 b)
{
	/*
	 * We consider all NANs to be equal and larger than any non-NAN. This is
	 * somewhat arbitrary; the important thing is to have a consistent sort
	 * order.
	 */
	if (isnan(a))
	{
		if (isnan(b))
			return 0;			/* NAN = NAN */
		else
			return 1;			/* NAN > non-NAN */
	}
	else if (isnan(b))
	{
		return -1;				/* non-NAN < NAN */
	}
	else
	{
		if (a > b)
			return 1;
		else if (a < b)
			return -1;
		else
			return 0;
	}
}

Datum
float4eq(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_cmp_internal(arg1, arg2) == 0);
}

Datum
float4ne(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_cmp_internal(arg1, arg2) != 0);
}

Datum
float4lt(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_cmp_internal(arg1, arg2) < 0);
}

Datum
float4le(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_cmp_internal(arg1, arg2) <= 0);
}

Datum
float4gt(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_cmp_internal(arg1, arg2) > 0);
}

Datum
float4ge(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_cmp_internal(arg1, arg2) >= 0);
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
static int
float8_cmp_internal(float8 a, float8 b)
{
	/*
	 * We consider all NANs to be equal and larger than any non-NAN. This is
	 * somewhat arbitrary; the important thing is to have a consistent sort
	 * order.
	 */
	if (isnan(a))
	{
		if (isnan(b))
			return 0;			/* NAN = NAN */
		else
			return 1;			/* NAN > non-NAN */
	}
	else if (isnan(b))
	{
		return -1;				/* non-NAN < NAN */
	}
	else
	{
		if (a > b)
			return 1;
		else if (a < b)
			return -1;
		else
			return 0;
	}
}

Datum
float8eq(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) == 0);
}

Datum
float8ne(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) != 0);
}

Datum
float8lt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) < 0);
}

Datum
float8le(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) <= 0);
}

Datum
float8gt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) > 0);
}

Datum
float8ge(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) >= 0);
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

	CHECKFLOATVAL((float4) num, isinf(num), num == 0);

	PG_RETURN_FLOAT4((float4) num);
}


/*
 *		dtoi4			- converts a float8 number to an int4 number
 */
Datum
dtoi4(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);
	int32		result;

	/* 'Inf' is handled by INT_MAX */
	if (num < INT_MIN || num > INT_MAX || isnan(num))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	result = (int32) rint(num);
	PG_RETURN_INT32(result);
}


/*
 *		dtoi2			- converts a float8 number to an int2 number
 */
Datum
dtoi2(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);

	if (num < SHRT_MIN || num > SHRT_MAX || isnan(num))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) rint(num));
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

	if (num < INT_MIN || num > INT_MAX || isnan(num))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32((int32) rint(num));
}


/*
 *		ftoi2			- converts a float4 number to an int2 number
 */
Datum
ftoi2(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);

	if (num < SHRT_MIN || num > SHRT_MAX || isnan(num))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) rint(num));
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

	CHECKFLOATVAL(result, isinf(arg1), arg1 == 0);
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
	CHECKFLOATVAL(result, isinf(arg1), arg1 == 0);
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
	 * pow() sets errno only on some platforms, depending on whether it
	 * follows _IEEE_, _POSIX_, _XOPEN_, or _SVID_, so we try to avoid using
	 * errno.  However, some platform/CPU combinations return errno == EDOM
	 * and result == Nan for negative arg1 and very large arg2 (they must be
	 * using something different from our floor() test to decide it's
	 * invalid).  Other platforms (HPPA) return errno == ERANGE and a large
	 * (HUGE_VAL) but finite result to signal overflow.
	 */
	errno = 0;
	result = pow(arg1, arg2);
	if (errno == EDOM && isnan(result))
	{
		if ((fabs(arg1) > 1 && arg2 >= 0) || (fabs(arg1) < 1 && arg2 < 0))
			/* The sign of Inf is not significant in this case. */
			result = get_float8_infinity();
		else if (fabs(arg1) != 1)
			result = 0;
		else
			result = 1;
	}
	else if (errno == ERANGE && result != 0 && !isinf(result))
		result = get_float8_infinity();

	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), arg1 == 0);
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

	errno = 0;
	result = exp(arg1);
	if (errno == ERANGE && result != 0 && !isinf(result))
		result = get_float8_infinity();

	CHECKFLOATVAL(result, isinf(arg1), false);
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

	CHECKFLOATVAL(result, isinf(arg1), arg1 == 1);
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

	CHECKFLOATVAL(result, isinf(arg1), arg1 == 1);
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

	CHECKFLOATVAL(result, false, true);
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

	CHECKFLOATVAL(result, false, true);
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

	CHECKFLOATVAL(result, false, true);
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

	CHECKFLOATVAL(result, false, true);
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

	CHECKFLOATVAL(result, false, true);
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
	CHECKFLOATVAL(result, true /* cot(0) == Inf */ , true);
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

	CHECKFLOATVAL(result, false, true);
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

	CHECKFLOATVAL(result, true /* tan(pi/2) == Inf */ , true);
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
 * in the degree-based trig functions are computed that way.
 *
 * The whole approach fails if init_degree_constants() gets inlined into the
 * call sites, since then constant-folding can happen anyway.  Currently it
 * seems sufficient to declare it non-static to prevent that.  We have no
 * expectation that other files will call this, but don't tell gcc that.
 *
 * Other hazards we are trying to forestall with this kluge include the
 * possibility that compilers will rearrange the expressions, or compute
 * some intermediate results in registers wider than a standard double.
 */
void
init_degree_constants(float8 thirty, float8 forty_five, float8 sixty,
					  float8 one_half, float8 one)
{
	sin_30 = sin(thirty * RADIANS_PER_DEGREE);
	one_minus_cos_60 = 1.0 - cos(sixty * RADIANS_PER_DEGREE);
	asin_0_5 = asin(one_half);
	acos_0_5 = acos(one_half);
	atan_1_0 = atan(one);
	tan_45 = sind_q1(forty_five) / cosd_q1(forty_five);
	cot_45 = cosd_q1(forty_five) / sind_q1(forty_five);
	degree_consts_set = true;
}

#define INIT_DEGREE_CONSTANTS() \
do { \
	if (!degree_consts_set) \
		init_degree_constants(30.0, 45.0, 60.0, 0.5, 1.0); \
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
		return (asin(x) / asin_0_5) * 30.0;
	else
		return 90.0 - (acos(x) / acos_0_5) * 60.0;
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
		return 90.0 - (asin(x) / asin_0_5) * 30.0;
	else
		return (acos(x) / acos_0_5) * 60.0;
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

	CHECKFLOATVAL(result, false, true);
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

	CHECKFLOATVAL(result, false, true);
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
	result = (atan(arg1) / atan_1_0) * 45.0;

	CHECKFLOATVAL(result, false, true);
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

	/* Per the POSIX spec, return NaN if either input is NaN */
	if (isnan(arg1) || isnan(arg2))
		PG_RETURN_FLOAT8(get_float8_nan());

	INIT_DEGREE_CONSTANTS();

	/*
	 * atan2d maps all inputs to values in the range [-180, 180], so the
	 * result should always be finite, even if the inputs are infinite.
	 */
	result = (atan2(arg1, arg2) / atan_1_0) * 45.0;

	CHECKFLOATVAL(result, false, true);
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
	return (sin(x * RADIANS_PER_DEGREE) / sin_30) / 2.0;
}


/*
 *		cosd_0_to_60	- returns the cosine of an angle that lies between 0
 *						  and 60 degrees.  This will return exactly 1 when x
 *						  is 0, and exactly 0.5 when x is 60 degrees.
 */
static double
cosd_0_to_60(double x)
{
	float8		one_minus_cos_x = 1.0 - cos(x * RADIANS_PER_DEGREE);

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

	CHECKFLOATVAL(result, false, true);
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

	result = sign * ((cosd_q1(arg1) / sind_q1(arg1)) / cot_45);

	/*
	 * On some machines we get cotd(270) = minus zero, but this isn't always
	 * true.  For portability, and because the user constituency for this
	 * function probably doesn't want minus zero, force it to plain zero.
	 */
	if (result == 0.0)
		result = 0.0;

	CHECKFLOATVAL(result, true /* cotd(0) == Inf */ , true);
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

	CHECKFLOATVAL(result, false, true);
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

	result = sign * ((sind_q1(arg1) / cosd_q1(arg1)) / tan_45);

	/*
	 * On some machines we get tand(180) = minus zero, but this isn't always
	 * true.  For portability, and because the user constituency for this
	 * function probably doesn't want minus zero, force it to plain zero.
	 */
	if (result == 0.0)
		result = 0.0;

	CHECKFLOATVAL(result, true /* tand(90) == Inf */ , true);
	PG_RETURN_FLOAT8(result);
}


/*
 *		degrees		- returns degrees converted from radians
 */
Datum
degrees(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	result = arg1 / RADIANS_PER_DEGREE;

	CHECKFLOATVAL(result, isinf(arg1), arg1 == 0);
	PG_RETURN_FLOAT8(result);
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
	float8		result;

	result = arg1 * RADIANS_PER_DEGREE;

	CHECKFLOATVAL(result, isinf(arg1), arg1 == 0);
	PG_RETURN_FLOAT8(result);
}


/*
 *		drandom		- returns a random number
 */
Datum
drandom(PG_FUNCTION_ARGS)
{
	float8		result;

	/* result [0.0 - 1.0) */
	result = (double) random() / ((double) MAX_RANDOM_VALUE + 1);

	PG_RETURN_FLOAT8(result);
}


/*
 *		setseed		- set seed for the random number generator
 */
Datum
setseed(PG_FUNCTION_ARGS)
{
	float8		seed = PG_GETARG_FLOAT8(0);
	int			iseed;

	if (seed < -1 || seed > 1)
		elog(ERROR, "setseed parameter %f out of range [-1,1]", seed);

	iseed = (int) (seed * MAX_RANDOM_VALUE);
	srandom((unsigned int) iseed);

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
 * The transition datatype for all these aggregates is a 3-element array
 * of float8, holding the values N, sum(X), sum(X*X) in that order.
 *
 * Note that we represent N as a float to avoid having to build a special
 * datatype.  Given a reasonable floating-point implementation, there should
 * be no accuracy loss unless N exceeds 2 ^ 52 or so (by which time the
 * user will have doubtless lost interest anyway...)
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

Datum
float8_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8		newval = PG_GETARG_FLOAT8(1);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumX2;

	transvalues = check_float8_array(transarray, "float8_accum", 3);
	N = transvalues[0];
	sumX = transvalues[1];
	sumX2 = transvalues[2];

	N += 1.0;
	sumX += newval;
	CHECKFLOATVAL(sumX, isinf(transvalues[1]) || isinf(newval), true);
	sumX2 += newval * newval;
	CHECKFLOATVAL(sumX2, isinf(transvalues[2]) || isinf(newval), true);

	/*
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we construct a
	 * new array with the updated transition data and return it.
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues[0] = N;
		transvalues[1] = sumX;
		transvalues[2] = sumX2;

		PG_RETURN_ARRAYTYPE_P(transarray);
	}
	else
	{
		Datum		transdatums[3];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(sumX);
		transdatums[2] = Float8GetDatumFast(sumX2);

		result = construct_array(transdatums, 3,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, 'd');

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
				sumX,
				sumX2;

	transvalues = check_float8_array(transarray, "float4_accum", 3);
	N = transvalues[0];
	sumX = transvalues[1];
	sumX2 = transvalues[2];

	N += 1.0;
	sumX += newval;
	CHECKFLOATVAL(sumX, isinf(transvalues[1]) || isinf(newval), true);
	sumX2 += newval * newval;
	CHECKFLOATVAL(sumX2, isinf(transvalues[2]) || isinf(newval), true);

	/*
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we construct a
	 * new array with the updated transition data and return it.
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues[0] = N;
		transvalues[1] = sumX;
		transvalues[2] = sumX2;

		PG_RETURN_ARRAYTYPE_P(transarray);
	}
	else
	{
		Datum		transdatums[3];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(sumX);
		transdatums[2] = Float8GetDatumFast(sumX2);

		result = construct_array(transdatums, 3,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, 'd');

		PG_RETURN_ARRAYTYPE_P(result);
	}
}

Datum
float8_avg(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX;

	transvalues = check_float8_array(transarray, "float8_avg", 3);
	N = transvalues[0];
	sumX = transvalues[1];
	/* ignore sumX2 */

	/* SQL defines AVG of no values to be NULL */
	if (N == 0.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(sumX / N);
}

Datum
float8_var_pop(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumX2,
				numerator;

	transvalues = check_float8_array(transarray, "float8_var_pop", 3);
	N = transvalues[0];
	sumX = transvalues[1];
	sumX2 = transvalues[2];

	/* Population variance is undefined when N is 0, so return NULL */
	if (N == 0.0)
		PG_RETURN_NULL();

	numerator = N * sumX2 - sumX * sumX;
	CHECKFLOATVAL(numerator, isinf(sumX2) || isinf(sumX), true);

	/* Watch out for roundoff error producing a negative numerator */
	if (numerator <= 0.0)
		PG_RETURN_FLOAT8(0.0);

	PG_RETURN_FLOAT8(numerator / (N * N));
}

Datum
float8_var_samp(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumX2,
				numerator;

	transvalues = check_float8_array(transarray, "float8_var_samp", 3);
	N = transvalues[0];
	sumX = transvalues[1];
	sumX2 = transvalues[2];

	/* Sample variance is undefined when N is 0 or 1, so return NULL */
	if (N <= 1.0)
		PG_RETURN_NULL();

	numerator = N * sumX2 - sumX * sumX;
	CHECKFLOATVAL(numerator, isinf(sumX2) || isinf(sumX), true);

	/* Watch out for roundoff error producing a negative numerator */
	if (numerator <= 0.0)
		PG_RETURN_FLOAT8(0.0);

	PG_RETURN_FLOAT8(numerator / (N * (N - 1.0)));
}

Datum
float8_stddev_pop(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumX2,
				numerator;

	transvalues = check_float8_array(transarray, "float8_stddev_pop", 3);
	N = transvalues[0];
	sumX = transvalues[1];
	sumX2 = transvalues[2];

	/* Population stddev is undefined when N is 0, so return NULL */
	if (N == 0.0)
		PG_RETURN_NULL();

	numerator = N * sumX2 - sumX * sumX;
	CHECKFLOATVAL(numerator, isinf(sumX2) || isinf(sumX), true);

	/* Watch out for roundoff error producing a negative numerator */
	if (numerator <= 0.0)
		PG_RETURN_FLOAT8(0.0);

	PG_RETURN_FLOAT8(sqrt(numerator / (N * N)));
}

Datum
float8_stddev_samp(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumX2,
				numerator;

	transvalues = check_float8_array(transarray, "float8_stddev_samp", 3);
	N = transvalues[0];
	sumX = transvalues[1];
	sumX2 = transvalues[2];

	/* Sample stddev is undefined when N is 0 or 1, so return NULL */
	if (N <= 1.0)
		PG_RETURN_NULL();

	numerator = N * sumX2 - sumX * sumX;
	CHECKFLOATVAL(numerator, isinf(sumX2) || isinf(sumX), true);

	/* Watch out for roundoff error producing a negative numerator */
	if (numerator <= 0.0)
		PG_RETURN_FLOAT8(0.0);

	PG_RETURN_FLOAT8(sqrt(numerator / (N * (N - 1.0))));
}

/*
 *		=========================
 *		SQL2003 BINARY AGGREGATES
 *		=========================
 *
 * The transition datatype for all these aggregates is a 6-element array of
 * float8, holding the values N, sum(X), sum(X*X), sum(Y), sum(Y*Y), sum(X*Y)
 * in that order.  Note that Y is the first argument to the aggregates!
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
				sumX,
				sumX2,
				sumY,
				sumY2,
				sumXY;

	transvalues = check_float8_array(transarray, "float8_regr_accum", 6);
	N = transvalues[0];
	sumX = transvalues[1];
	sumX2 = transvalues[2];
	sumY = transvalues[3];
	sumY2 = transvalues[4];
	sumXY = transvalues[5];

	N += 1.0;
	sumX += newvalX;
	CHECKFLOATVAL(sumX, isinf(transvalues[1]) || isinf(newvalX), true);
	sumX2 += newvalX * newvalX;
	CHECKFLOATVAL(sumX2, isinf(transvalues[2]) || isinf(newvalX), true);
	sumY += newvalY;
	CHECKFLOATVAL(sumY, isinf(transvalues[3]) || isinf(newvalY), true);
	sumY2 += newvalY * newvalY;
	CHECKFLOATVAL(sumY2, isinf(transvalues[4]) || isinf(newvalY), true);
	sumXY += newvalX * newvalY;
	CHECKFLOATVAL(sumXY, isinf(transvalues[5]) || isinf(newvalX) ||
				  isinf(newvalY), true);

	/*
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we construct a
	 * new array with the updated transition data and return it.
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues[0] = N;
		transvalues[1] = sumX;
		transvalues[2] = sumX2;
		transvalues[3] = sumY;
		transvalues[4] = sumY2;
		transvalues[5] = sumXY;

		PG_RETURN_ARRAYTYPE_P(transarray);
	}
	else
	{
		Datum		transdatums[6];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(sumX);
		transdatums[2] = Float8GetDatumFast(sumX2);
		transdatums[3] = Float8GetDatumFast(sumY);
		transdatums[4] = Float8GetDatumFast(sumY2);
		transdatums[5] = Float8GetDatumFast(sumXY);

		result = construct_array(transdatums, 6,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, 'd');

		PG_RETURN_ARRAYTYPE_P(result);
	}
}

Datum
float8_regr_sxx(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumX2,
				numerator;

	transvalues = check_float8_array(transarray, "float8_regr_sxx", 6);
	N = transvalues[0];
	sumX = transvalues[1];
	sumX2 = transvalues[2];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	numerator = N * sumX2 - sumX * sumX;
	CHECKFLOATVAL(numerator, isinf(sumX2) || isinf(sumX), true);

	/* Watch out for roundoff error producing a negative numerator */
	if (numerator <= 0.0)
		PG_RETURN_FLOAT8(0.0);

	PG_RETURN_FLOAT8(numerator / N);
}

Datum
float8_regr_syy(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumY,
				sumY2,
				numerator;

	transvalues = check_float8_array(transarray, "float8_regr_syy", 6);
	N = transvalues[0];
	sumY = transvalues[3];
	sumY2 = transvalues[4];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	numerator = N * sumY2 - sumY * sumY;
	CHECKFLOATVAL(numerator, isinf(sumY2) || isinf(sumY), true);

	/* Watch out for roundoff error producing a negative numerator */
	if (numerator <= 0.0)
		PG_RETURN_FLOAT8(0.0);

	PG_RETURN_FLOAT8(numerator / N);
}

Datum
float8_regr_sxy(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumY,
				sumXY,
				numerator;

	transvalues = check_float8_array(transarray, "float8_regr_sxy", 6);
	N = transvalues[0];
	sumX = transvalues[1];
	sumY = transvalues[3];
	sumXY = transvalues[5];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	numerator = N * sumXY - sumX * sumY;
	CHECKFLOATVAL(numerator, isinf(sumXY) || isinf(sumX) ||
				  isinf(sumY), true);

	/* A negative result is valid here */

	PG_RETURN_FLOAT8(numerator / N);
}

Datum
float8_regr_avgx(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX;

	transvalues = check_float8_array(transarray, "float8_regr_avgx", 6);
	N = transvalues[0];
	sumX = transvalues[1];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(sumX / N);
}

Datum
float8_regr_avgy(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumY;

	transvalues = check_float8_array(transarray, "float8_regr_avgy", 6);
	N = transvalues[0];
	sumY = transvalues[3];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(sumY / N);
}

Datum
float8_covar_pop(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumY,
				sumXY,
				numerator;

	transvalues = check_float8_array(transarray, "float8_covar_pop", 6);
	N = transvalues[0];
	sumX = transvalues[1];
	sumY = transvalues[3];
	sumXY = transvalues[5];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	numerator = N * sumXY - sumX * sumY;
	CHECKFLOATVAL(numerator, isinf(sumXY) || isinf(sumX) ||
				  isinf(sumY), true);

	PG_RETURN_FLOAT8(numerator / (N * N));
}

Datum
float8_covar_samp(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumY,
				sumXY,
				numerator;

	transvalues = check_float8_array(transarray, "float8_covar_samp", 6);
	N = transvalues[0];
	sumX = transvalues[1];
	sumY = transvalues[3];
	sumXY = transvalues[5];

	/* if N is <= 1 we should return NULL */
	if (N < 2.0)
		PG_RETURN_NULL();

	numerator = N * sumXY - sumX * sumY;
	CHECKFLOATVAL(numerator, isinf(sumXY) || isinf(sumX) ||
				  isinf(sumY), true);

	PG_RETURN_FLOAT8(numerator / (N * (N - 1.0)));
}

Datum
float8_corr(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumX2,
				sumY,
				sumY2,
				sumXY,
				numeratorX,
				numeratorY,
				numeratorXY;

	transvalues = check_float8_array(transarray, "float8_corr", 6);
	N = transvalues[0];
	sumX = transvalues[1];
	sumX2 = transvalues[2];
	sumY = transvalues[3];
	sumY2 = transvalues[4];
	sumXY = transvalues[5];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	numeratorX = N * sumX2 - sumX * sumX;
	CHECKFLOATVAL(numeratorX, isinf(sumX2) || isinf(sumX), true);
	numeratorY = N * sumY2 - sumY * sumY;
	CHECKFLOATVAL(numeratorY, isinf(sumY2) || isinf(sumY), true);
	numeratorXY = N * sumXY - sumX * sumY;
	CHECKFLOATVAL(numeratorXY, isinf(sumXY) || isinf(sumX) ||
				  isinf(sumY), true);
	if (numeratorX <= 0 || numeratorY <= 0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(numeratorXY / sqrt(numeratorX * numeratorY));
}

Datum
float8_regr_r2(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumX2,
				sumY,
				sumY2,
				sumXY,
				numeratorX,
				numeratorY,
				numeratorXY;

	transvalues = check_float8_array(transarray, "float8_regr_r2", 6);
	N = transvalues[0];
	sumX = transvalues[1];
	sumX2 = transvalues[2];
	sumY = transvalues[3];
	sumY2 = transvalues[4];
	sumXY = transvalues[5];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	numeratorX = N * sumX2 - sumX * sumX;
	CHECKFLOATVAL(numeratorX, isinf(sumX2) || isinf(sumX), true);
	numeratorY = N * sumY2 - sumY * sumY;
	CHECKFLOATVAL(numeratorY, isinf(sumY2) || isinf(sumY), true);
	numeratorXY = N * sumXY - sumX * sumY;
	CHECKFLOATVAL(numeratorXY, isinf(sumXY) || isinf(sumX) ||
				  isinf(sumY), true);
	if (numeratorX <= 0)
		PG_RETURN_NULL();
	/* per spec, horizontal line produces 1.0 */
	if (numeratorY <= 0)
		PG_RETURN_FLOAT8(1.0);

	PG_RETURN_FLOAT8((numeratorXY * numeratorXY) /
					 (numeratorX * numeratorY));
}

Datum
float8_regr_slope(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumX2,
				sumY,
				sumXY,
				numeratorX,
				numeratorXY;

	transvalues = check_float8_array(transarray, "float8_regr_slope", 6);
	N = transvalues[0];
	sumX = transvalues[1];
	sumX2 = transvalues[2];
	sumY = transvalues[3];
	sumXY = transvalues[5];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	numeratorX = N * sumX2 - sumX * sumX;
	CHECKFLOATVAL(numeratorX, isinf(sumX2) || isinf(sumX), true);
	numeratorXY = N * sumXY - sumX * sumY;
	CHECKFLOATVAL(numeratorXY, isinf(sumXY) || isinf(sumX) ||
				  isinf(sumY), true);
	if (numeratorX <= 0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(numeratorXY / numeratorX);
}

Datum
float8_regr_intercept(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				sumX,
				sumX2,
				sumY,
				sumXY,
				numeratorX,
				numeratorXXY;

	transvalues = check_float8_array(transarray, "float8_regr_intercept", 6);
	N = transvalues[0];
	sumX = transvalues[1];
	sumX2 = transvalues[2];
	sumY = transvalues[3];
	sumXY = transvalues[5];

	/* if N is 0 we should return NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	numeratorX = N * sumX2 - sumX * sumX;
	CHECKFLOATVAL(numeratorX, isinf(sumX2) || isinf(sumX), true);
	numeratorXXY = sumY * sumX2 - sumX * sumXY;
	CHECKFLOATVAL(numeratorXXY, isinf(sumY) || isinf(sumX2) ||
				  isinf(sumX) || isinf(sumXY), true);
	if (numeratorX <= 0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(numeratorXXY / numeratorX);
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
	float8		result;

	result = arg1 + arg2;
	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), true);
	PG_RETURN_FLOAT8(result);
}

Datum
float48mi(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	result = arg1 - arg2;
	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), true);
	PG_RETURN_FLOAT8(result);
}

Datum
float48mul(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	result = arg1 * arg2;
	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2),
				  arg1 == 0 || arg2 == 0);
	PG_RETURN_FLOAT8(result);
}

Datum
float48div(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	if (arg2 == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = arg1 / arg2;
	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), arg1 == 0);
	PG_RETURN_FLOAT8(result);
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
	float8		result;

	result = arg1 + arg2;

	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), true);
	PG_RETURN_FLOAT8(result);
}

Datum
float84mi(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);
	float8		result;

	result = arg1 - arg2;

	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), true);
	PG_RETURN_FLOAT8(result);
}

Datum
float84mul(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);
	float8		result;

	result = arg1 * arg2;

	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2),
				  arg1 == 0 || arg2 == 0);
	PG_RETURN_FLOAT8(result);
}

Datum
float84div(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);
	float8		result;

	if (arg2 == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = arg1 / arg2;

	CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), arg1 == 0);
	PG_RETURN_FLOAT8(result);
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

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) == 0);
}

Datum
float48ne(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) != 0);
}

Datum
float48lt(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) < 0);
}

Datum
float48le(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) <= 0);
}

Datum
float48gt(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) > 0);
}

Datum
float48ge(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) >= 0);
}

/*
 *		float84{eq,ne,lt,le,gt,ge}		- float8/float4 comparison operations
 */
Datum
float84eq(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) == 0);
}

Datum
float84ne(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) != 0);
}

Datum
float84lt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) < 0);
}

Datum
float84le(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) <= 0);
}

Datum
float84gt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) > 0);
}

Datum
float84ge(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_cmp_internal(arg1, arg2) >= 0);
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
			result = count + 1;
			/* check for overflow */
			if (result < count)
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
			result = count + 1;
			/* check for overflow */
			if (result < count)
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

/* ========== PRIVATE ROUTINES ========== */

#ifndef HAVE_CBRT

static double
cbrt(double x)
{
	int			isneg = (x < 0.0);
	double		absx = fabs(x);
	double		tmpres = pow(absx, (double) 1.0 / (double) 3.0);

	/*
	 * The result is somewhat inaccurate --- not really pow()'s fault, as the
	 * exponent it's handed contains roundoff error.  We can improve the
	 * accuracy by doing one iteration of Newton's formula.  Beware of zero
	 * input however.
	 */
	if (tmpres > 0.0)
		tmpres -= (tmpres - absx / (tmpres * tmpres)) / (double) 3.0;

	return isneg ? -tmpres : tmpres;
}

#endif   /* !HAVE_CBRT */
