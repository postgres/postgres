/*-------------------------------------------------------------------------
 *
 * float.c--
 *	  Functions for the built-in floating-point types.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/float.c,v 1.33 1998/09/01 04:32:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * OLD COMMENTS
 *		Basic float4 ops:
 *		 float4in, float4out, float4abs, float4um
 *		Basic float8 ops:
 *		 float8in, float8inAd, float8out, float8outAd, float8abs, float8um
 *		Arithmetic operators:
 *		 float4pl, float4mi, float4mul, float4div
 *		 float8pl, float8mi, float8mul, float8div
 *		Comparison operators:
 *		 float4eq, float4ne, float4lt, float4le, float4gt, float4ge
 *		 float8eq, float8ne, float8lt, float8le, float8gt, float8ge
 *		Conversion routines:
 *		 ftod, dtof, i4tod, dtoi4, i2tod, dtoi2, itof, ftoi, i2tof, ftoi2
 *
 *		Random float8 ops:
 *		 dround, dtrunc, dsqrt, dcbrt, dpow, dexp, dlog1
 *		Arithmetic operators:
 *		 float48pl, float48mi, float48mul, float48div
 *		 float84pl, float84mi, float84mul, float84div
 *		Comparison operators:
 *		 float48eq, float48ne, float48lt, float48le, float48gt, float48ge
 *		 float84eq, float84ne, float84lt, float84le, float84gt, float84ge
 *
 *		(You can do the arithmetic and comparison stuff using conversion
 *		 routines, but then you pay the overhead of converting...)
 *
 * XXX GLUESOME STUFF. FIX IT! -AY '94
 *
 *		Added some additional conversion routines and cleaned up
 *		 a bit of the existing code. Need to change the error checking
 *		 for calls to pow(), exp() since on some machines (my Linux box
 *		 included) these routines do not set errno. - tgl 97/05/10
 */
#include <stdio.h>				/* for sprintf() */
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>

#include <float.h>				/* faked on sunos4 */

#include <math.h>

#include "postgres.h"
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#include "fmgr.h"
#include "utils/builtins.h"		/* for ftod() prototype */
#include "utils/palloc.h"


#ifndef SHRT_MAX
#define SHRT_MAX 32767
#endif
#ifndef SHRT_MIN
#define SHRT_MIN (-32768)
#endif

#define FORMAT			'g'		/* use "g" output format as standard
								 * format */
/* not sure what the following should be, but better to make it over-sufficient */
#define MAXFLOATWIDTH	64
#define MAXDOUBLEWIDTH	128

#if !(NeXT && NX_CURRENT_COMPILER_RELEASE > NX_COMPILER_RELEASE_3_2)
 /* NS3.3 has conflicting declarations of these in <math.h> */

#ifndef atof
extern double atof(const char *p);

#endif

#ifndef HAVE_CBRT
#define cbrt my_cbrt
static double cbrt(double x);

#else
#if !defined(nextstep)
extern double cbrt(double x);

#endif
#endif

#ifndef HAVE_RINT
#define rint my_rint
static double rint(double x);

#else
extern double rint(double x);

#endif

extern int	isinf(double x);

#endif
/* ========== USER I/O ROUTINES ========== */


#define FLOAT4_MAX		 FLT_MAX
#define FLOAT4_MIN		 FLT_MIN
#define FLOAT8_MAX		 DBL_MAX
#define FLOAT8_MIN		 DBL_MIN

/*
 * if FLOAT8_MIN and FLOAT8_MAX are the limits of the range a
 * double can store, then how are we ever going to wind up
 * with something stored in a double that is outside those
 * limits?	(and similarly for FLOAT4_{MIN,MAX}/float.)
 * doesn't make sense to me, and it causes a
 * floating point exception on linuxalpha, so UNSAFE_FLOATS
 * it is.
 * (maybe someone wanted to allow for values other than DBL_MIN/
 * DBL_MAX for FLOAT8_MIN/FLOAT8_MAX?)
 *								--djm 12/12/96
 * according to Richard Henderson this is a known bug in gcc on
 * the Alpha.  might as well leave the workaround in
 * until the distributions are updated.
 *								--djm 12/16/96
 */
#if ( defined(linux) && defined(__alpha) ) && !defined(UNSAFE_FLOATS)
#define UNSAFE_FLOATS
#endif

/*
   check to see if a float4 val is outside of
   the FLOAT4_MIN, FLOAT4_MAX bounds.

   raise an elog warning if it is
*/
static void
CheckFloat4Val(double val)
{

	/*
	 * defining unsafe floats's will make float4 and float8 ops faster at
	 * the cost of safety, of course!
	 */
#ifdef UNSAFE_FLOATS
	return;
#else
	if (fabs(val) > FLOAT4_MAX)
		elog(ERROR, "Bad float4 input format -- overflow");
	if (val != 0.0 && fabs(val) < FLOAT4_MIN)
		elog(ERROR, "Bad float4 input format -- underflow");
	return;
#endif	 /* UNSAFE_FLOATS */
}

/*
   check to see if a float8 val is outside of
   the FLOAT8_MIN, FLOAT8_MAX bounds.

   raise an elog warning if it is
*/
void
CheckFloat8Val(double val)
{

	/*
	 * defining unsafe floats's will make float4 and float8 ops faster at
	 * the cost of safety, of course!
	 */
#ifdef UNSAFE_FLOATS
	return;
#else
	if (fabs(val) > FLOAT8_MAX)
		elog(ERROR, "Bad float8 input format -- overflow");
	if (val != 0.0 && fabs(val) < FLOAT8_MIN)
		elog(ERROR, "Bad float8 input format -- underflow");
	return;
#endif	 /* UNSAFE_FLOATS */
}

/*
 *		float4in		- converts "num" to float
 *						  restricted syntax:
 *						  {<sp>} [+|-] {digit} [.{digit}] [<exp>]
 *						  where <sp> is a space, digit is 0-9,
 *						  <exp> is "e" or "E" followed by an integer.
 */
float32
float4in(char *num)
{
	float32		result = (float32) palloc(sizeof(float32data));
	double		val;
	char	   *endptr;

	errno = 0;
	val = strtod(num, &endptr);
	if (*endptr != '\0' || errno == ERANGE)
		elog(ERROR, "Bad float4 input format '%s'", num);

	/*
	 * if we get here, we have a legal double, still need to check to see
	 * if it's a legal float
	 */

	CheckFloat4Val(val);

	*result = val;
	return result;
}

/*
 *		float4out		- converts a float4 number to a string
 *						  using a standard output format
 */
char *
float4out(float32 num)
{
	char	   *ascii = (char *) palloc(MAXFLOATWIDTH + 1);

	if (!num)
		return strcpy(ascii, "(null)");

	sprintf(ascii, "%.*g", FLT_DIG, *num);
	return ascii;
}


/*
 *		float8in		- converts "num" to float8
 *						  restricted syntax:
 *						  {<sp>} [+|-] {digit} [.{digit}] [<exp>]
 *						  where <sp> is a space, digit is 0-9,
 *						  <exp> is "e" or "E" followed by an integer.
 */
float64
float8in(char *num)
{
	float64		result = (float64) palloc(sizeof(float64data));
	double		val;
	char	   *endptr;

	errno = 0;
	val = strtod(num, &endptr);
	if (*endptr != '\0' || errno == ERANGE)
		elog(ERROR, "Bad float8 input format '%s'", num);

	CheckFloat8Val(val);
	*result = val;
	return result;
}


/*
 *		float8out		- converts float8 number to a string
 *						  using a standard output format
 */
char *
float8out(float64 num)
{
	char	   *ascii = (char *) palloc(MAXDOUBLEWIDTH + 1);

	if (!num)
		return strcpy(ascii, "(null)");

	if (isnan(*num))
		return strcpy(ascii, "NaN");
	if (isinf(*num))
		return strcpy(ascii, "Infinity");

	sprintf(ascii, "%.*g", DBL_DIG, *num);
	return ascii;
}

/* ========== PUBLIC ROUTINES ========== */


/*
 *		======================
 *		FLOAT4 BASE OPERATIONS
 *		======================
 */

/*
 *		float4abs		- returns a pointer to |arg1| (absolute value)
 */
float32
float4abs(float32 arg1)
{
	float32		result;
	double		val;

	if (!arg1)
		return (float32) NULL;

	val = fabs(*arg1);

	CheckFloat4Val(val);

	result = (float32) palloc(sizeof(float32data));
	*result = val;
	return result;
}

/*
 *		float4um		- returns a pointer to -arg1 (unary minus)
 */
float32
float4um(float32 arg1)
{
	float32		result;
	double		val;

	if (!arg1)
		return (float32) NULL;

	val = ((*arg1 != 0) ? -(*arg1) : *arg1);
	CheckFloat4Val(val);

	result = (float32) palloc(sizeof(float32data));
	*result = val;
	return result;
}

float32
float4larger(float32 arg1, float32 arg2)
{
	float32		result;

	if (!arg1 || !arg2)
		return (float32) NULL;

	result = (float32) palloc(sizeof(float32data));

	*result = ((*arg1 > *arg2) ? *arg1 : *arg2);
	return result;
}

float32
float4smaller(float32 arg1, float32 arg2)
{
	float32		result;

	if (!arg1 || !arg2)
		return (float32) NULL;

	result = (float32) palloc(sizeof(float32data));

	*result = ((*arg1 > *arg2) ? *arg2 : *arg1);
	return result;
}

/*
 *		======================
 *		FLOAT8 BASE OPERATIONS
 *		======================
 */

/*
 *		float8abs		- returns a pointer to |arg1| (absolute value)
 */
float64
float8abs(float64 arg1)
{
	float64		result;
	double		val;

	if (!arg1)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	val = fabs(*arg1);
	CheckFloat8Val(val);
	*result = val;
	return result;
}


/*
 *		float8um		- returns a pointer to -arg1 (unary minus)
 */
float64
float8um(float64 arg1)
{
	float64		result;
	double		val;

	if (!arg1)
		return (float64) NULL;

	val = ((*arg1 != 0) ? -(*arg1) : *arg1);

	CheckFloat8Val(val);
	result = (float64) palloc(sizeof(float64data));
	*result = val;
	return result;
}

float64
float8larger(float64 arg1, float64 arg2)
{
	float64		result;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	*result = ((*arg1 > *arg2) ? *arg1 : *arg2);
	return result;
}

float64
float8smaller(float64 arg1, float64 arg2)
{
	float64		result;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	*result = ((*arg1 > *arg2) ? *arg2 : *arg1);
	return result;
}


/*
 *		====================
 *		ARITHMETIC OPERATORS
 *		====================
 */

/*
 *		float4pl		- returns a pointer to arg1 + arg2
 *		float4mi		- returns a pointer to arg1 - arg2
 *		float4mul		- returns a pointer to arg1 * arg2
 *		float4div		- returns a pointer to arg1 / arg2
 *		float4inc		- returns a poniter to arg1 + 1.0
 */
float32
float4pl(float32 arg1, float32 arg2)
{
	float32		result;
	double		val;

	if (!arg1 || !arg2)
		return (float32) NULL;

	val = *arg1 + *arg2;
	CheckFloat4Val(val);

	result = (float32) palloc(sizeof(float32data));
	*result = val;

	return result;
}

float32
float4mi(float32 arg1, float32 arg2)
{
	float32		result;
	double		val;

	if (!arg1 || !arg2)
		return (float32) NULL;

	val = *arg1 - *arg2;

	CheckFloat4Val(val);
	result = (float32) palloc(sizeof(float32data));
	*result = val;
	return result;
}

float32
float4mul(float32 arg1, float32 arg2)
{
	float32		result;
	double		val;

	if (!arg1 || !arg2)
		return (float32) NULL;

	val = *arg1 * *arg2;

	CheckFloat4Val(val);
	result = (float32) palloc(sizeof(float32data));
	*result = val;
	return result;
}

float32
float4div(float32 arg1, float32 arg2)
{
	float32		result;
	double		val;

	if (!arg1 || !arg2)
		return (float32) NULL;

	if (*arg2 == 0.0)
		elog(ERROR, "float4div: divide by zero error");

	val = *arg1 / *arg2;

	CheckFloat4Val(val);
	result = (float32) palloc(sizeof(float32data));
	*result = *arg1 / *arg2;
	return result;
}

float32
float4inc(float32 arg1)
{
	double		val;

	if (!arg1)
		return (float32) NULL;

	val = *arg1 + (float32data) 1.0;
	CheckFloat4Val(val);
	*arg1 = val;
	return arg1;
}

/*
 *		float8pl		- returns a pointer to arg1 + arg2
 *		float8mi		- returns a pointer to arg1 - arg2
 *		float8mul		- returns a pointer to arg1 * arg2
 *		float8div		- returns a pointer to arg1 / arg2
 *		float8inc		- returns a pointer to arg1 + 1.0
 */
float64
float8pl(float64 arg1, float64 arg2)
{
	float64		result;
	double		val;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	val = *arg1 + *arg2;
	CheckFloat8Val(val);
	*result = val;
	return result;
}

float64
float8mi(float64 arg1, float64 arg2)
{
	float64		result;
	double		val;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	val = *arg1 - *arg2;
	CheckFloat8Val(val);
	*result = val;
	return result;
}

float64
float8mul(float64 arg1, float64 arg2)
{
	float64		result;
	double		val;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	val = *arg1 * *arg2;
	CheckFloat8Val(val);
	*result = val;
	return result;
}

float64
float8div(float64 arg1, float64 arg2)
{
	float64		result;
	double		val;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	if (*arg2 == 0.0)
		elog(ERROR, "float8div: divide by zero error");

	val = *arg1 / *arg2;
	CheckFloat8Val(val);
	*result = val;
	return result;
}

float64
float8inc(float64 arg1)
{
	double		val;

	if (!arg1)
		return (float64) NULL;

	val = *arg1 + (float64data) 1.0;
	CheckFloat8Val(val);
	*arg1 = val;
	return arg1;
}


/*
 *		====================
 *		COMPARISON OPERATORS
 *		====================
 */

/*
 *		float4{eq,ne,lt,le,gt,ge}		- float4/float4 comparison operations
 */
bool
float4eq(float32 arg1, float32 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 == *arg2;
}

bool
float4ne(float32 arg1, float32 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 != *arg2;
}

bool
float4lt(float32 arg1, float32 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 < *arg2;
}

bool
float4le(float32 arg1, float32 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 <= *arg2;
}

bool
float4gt(float32 arg1, float32 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 > *arg2;
}

bool
float4ge(float32 arg1, float32 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 >= *arg2;
}

/*
 *		float8{eq,ne,lt,le,gt,ge}		- float8/float8 comparison operations
 */
bool
float8eq(float64 arg1, float64 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 == *arg2;
}

bool
float8ne(float64 arg1, float64 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 != *arg2;
}

bool
float8lt(float64 arg1, float64 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 < *arg2;
}

bool
float8le(float64 arg1, float64 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 <= *arg2;
}

bool
float8gt(float64 arg1, float64 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 > *arg2;
}

bool
float8ge(float64 arg1, float64 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 >= *arg2;
}


/*
 *		===================
 *		CONVERSION ROUTINES
 *		===================
 */

/*
 *		ftod			- converts a float4 number to a float8 number
 */
float64
ftod(float32 num)
{
	float64		result;

	if (!num)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	*result = *num;
	return result;
}


/*
 *		dtof			- converts a float8 number to a float4 number
 */
float32
dtof(float64 num)
{
	float32		result;

	if (!num)
		return (float32) NULL;

	CheckFloat4Val(*num);

	result = (float32) palloc(sizeof(float32data));

	*result = *num;
	return result;
}


/*
 *		dtoi4			- converts a float8 number to an int4 number
 */
int32
dtoi4(float64 num)
{
	int32		result;

	if (!PointerIsValid(num))
		elog(ERROR, "dtoi4: unable to convert null", NULL);

	if ((*num < INT_MIN) || (*num > INT_MAX))
		elog(ERROR, "dtoi4: integer out of range", NULL);

	result = rint(*num);
	return result;
}


/*
 *		dtoi2			- converts a float8 number to an int2 number
 */
int16
dtoi2(float64 num)
{
	int16		result;

	if (!PointerIsValid(num))
		elog(ERROR, "dtoi2: unable to convert null", NULL);

	if ((*num < SHRT_MIN) || (*num > SHRT_MAX))
		elog(ERROR, "dtoi2: integer out of range", NULL);

	result = rint(*num);
	return result;
}


/*
 *		i4tod			- converts an int4 number to a float8 number
 */
float64
i4tod(int32 num)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));

	*result = num;
	return result;
}


/*
 *		i2tod			- converts an int2 number to a float8 number
 */
float64
i2tod(int16 num)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));

	*result = num;
	return result;
}


/*
 *		ftoi4			- converts a float8 number to an int4 number
 */
int32
ftoi4(float32 num)
{
	int32		result;

	if (!PointerIsValid(num))
		elog(ERROR, "ftoi4: unable to convert null", NULL);

	if ((*num < INT_MIN) || (*num > INT_MAX))
		elog(ERROR, "ftoi4: integer out of range", NULL);

	result = rint(*num);
	return result;
}


/*
 *		ftoi2			- converts a float8 number to an int2 number
 */
int16
ftoi2(float32 num)
{
	int16		result;

	if (!PointerIsValid(num))
		elog(ERROR, "ftoi2: unable to convert null", NULL);

	if ((*num < SHRT_MIN) || (*num > SHRT_MAX))
		elog(ERROR, "ftoi2: integer out of range", NULL);

	result = rint(*num);
	return result;
}


/*
 *		i4tof			- converts an int4 number to a float8 number
 */
float32
i4tof(int32 num)
{
	float32		result;

	result = (float32) palloc(sizeof(float32data));

	*result = num;
	return result;
}


/*
 *		i2tof			- converts an int2 number to a float8 number
 */
float32
i2tof(int16 num)
{
	float32		result;

	result = (float32) palloc(sizeof(float32data));

	*result = num;
	return result;
}


/*
 *		=======================
 *		RANDOM FLOAT8 OPERATORS
 *		=======================
 */

/*
 *		dround			- returns a pointer to	ROUND(arg1)
 */
float64
dround(float64 arg1)
{
	float64		result;
	double		tmp;

	if (!arg1)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp = *arg1;
	*result = (float64data) rint(tmp);
	return result;
}


/*
 *		dtrunc			- returns a pointer to	truncation of arg1,
 *						  arg1 >= 0 ... the greatest integer as float8 less
 *										than or equal to arg1
 *						  arg1 < 0	... the greatest integer as float8 greater
 *										than or equal to arg1
 */
float64
dtrunc(float64 arg1)
{
	float64		result;
	double		tmp;

	if (!arg1)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp = *arg1;
	if (*arg1 >= 0)
		*result = (float64data) floor(tmp);
	else
		*result = (float64data) -(floor(-tmp));
	return result;
}


/*
 *		dsqrt			- returns a pointer to square root of arg1
 */
float64
dsqrt(float64 arg1)
{
	float64		result;
	double		tmp;

	if (!arg1)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp = *arg1;
	*result = (float64data) sqrt(tmp);
	return result;
}


/*
 *		dcbrt			- returns a pointer to cube root of arg1
 */
float64
dcbrt(float64 arg1)
{
	float64		result;
	double		tmp;

	if (!arg1)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp = *arg1;
	*result = (float64data) cbrt(tmp);
	return result;
}


/*
 *		dpow			- returns a pointer to pow(arg1,arg2)
 */
float64
dpow(float64 arg1, float64 arg2)
{
	float64		result;
	double		tmp1,
				tmp2;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp1 = *arg1;
	tmp2 = *arg2;
#ifndef finite
	errno = 0;
#endif
	*result = (float64data) pow(tmp1, tmp2);
#ifndef finite
	if (errno == ERANGE)
#else
	if (!finite(*result))
#endif
		elog(ERROR, "pow() result is out of range");

	CheckFloat8Val(*result);
	return result;
}


/*
 *		dexp			- returns a pointer to the exponential function of arg1
 */
float64
dexp(float64 arg1)
{
	float64		result;
	double		tmp;

	if (!arg1)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp = *arg1;
#ifndef finite
	errno = 0;
#endif
	*result = (float64data) exp(tmp);
#ifndef finite
	if (errno == ERANGE)
#else
	if (!finite(*result))
#endif
		elog(ERROR, "exp() result is out of range");

	CheckFloat8Val(*result);
	return result;
}


/*
 *		dlog1			- returns a pointer to the natural logarithm of arg1
 *						  ("dlog" is already a logging routine...)
 */
float64
dlog1(float64 arg1)
{
	float64		result;
	double		tmp;

	if (!arg1)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp = *arg1;
	if (tmp == 0.0)
		elog(ERROR, "can't take log of zero");
	if (tmp < 0)
		elog(ERROR, "can't take log of a negative number");
	*result = (float64data) log(tmp);

	CheckFloat8Val(*result);
	return result;
}


/*
 *		====================
 *		ARITHMETIC OPERATORS
 *		====================
 */

/*
 *		float48pl		- returns a pointer to arg1 + arg2
 *		float48mi		- returns a pointer to arg1 - arg2
 *		float48mul		- returns a pointer to arg1 * arg2
 *		float48div		- returns a pointer to arg1 / arg2
 */
float64
float48pl(float32 arg1, float64 arg2)
{
	float64		result;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	*result = *arg1 + *arg2;
	CheckFloat8Val(*result);
	return result;
}

float64
float48mi(float32 arg1, float64 arg2)
{
	float64		result;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	*result = *arg1 - *arg2;
	CheckFloat8Val(*result);
	return result;
}

float64
float48mul(float32 arg1, float64 arg2)
{
	float64		result;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	*result = *arg1 * *arg2;
	CheckFloat8Val(*result);
	return result;
}

float64
float48div(float32 arg1, float64 arg2)
{
	float64		result;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	if (*arg2 == 0.0)
		elog(ERROR, "float48div: divide by zero");

	*result = *arg1 / *arg2;
	CheckFloat8Val(*result);
	return result;
}

/*
 *		float84pl		- returns a pointer to arg1 + arg2
 *		float84mi		- returns a pointer to arg1 - arg2
 *		float84mul		- returns a pointer to arg1 * arg2
 *		float84div		- returns a pointer to arg1 / arg2
 */
float64
float84pl(float64 arg1, float32 arg2)
{
	float64		result;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	*result = *arg1 + *arg2;
	CheckFloat8Val(*result);
	return result;
}

float64
float84mi(float64 arg1, float32 arg2)
{
	float64		result;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	*result = *arg1 - *arg2;
	CheckFloat8Val(*result);
	return result;
}

float64
float84mul(float64 arg1, float32 arg2)
{

	float64		result;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	*result = *arg1 * *arg2;
	CheckFloat8Val(*result);
	return result;
}

float64
float84div(float64 arg1, float32 arg2)
{
	float64		result;

	if (!arg1 || !arg2)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	if (*arg2 == 0.0)
		elog(ERROR, "float48div: divide by zero");

	*result = *arg1 / *arg2;
	CheckFloat8Val(*result);
	return result;
}

/*
 *		====================
 *		COMPARISON OPERATORS
 *		====================
 */

/*
 *		float48{eq,ne,lt,le,gt,ge}		- float4/float8 comparison operations
 */
bool
float48eq(float32 arg1, float64 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 == (float) *arg2;
}

bool
float48ne(float32 arg1, float64 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 != (float) *arg2;
}

bool
float48lt(float32 arg1, float64 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 < (float) *arg2;
}

bool
float48le(float32 arg1, float64 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 <= (float) *arg2;
}

bool
float48gt(float32 arg1, float64 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 > (float) *arg2;
}

bool
float48ge(float32 arg1, float64 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return *arg1 >= (float) *arg2;
}

/*
 *		float84{eq,ne,lt,le,gt,ge}		- float4/float8 comparison operations
 */
bool
float84eq(float64 arg1, float32 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return (float) *arg1 == *arg2;
}

bool
float84ne(float64 arg1, float32 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return (float) *arg1 != *arg2;
}

bool
float84lt(float64 arg1, float32 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return (float) *arg1 < *arg2;
}

bool
float84le(float64 arg1, float32 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return (float) *arg1 <= *arg2;
}

bool
float84gt(float64 arg1, float32 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return (float) *arg1 > *arg2;
}

bool
float84ge(float64 arg1, float32 arg2)
{
	if (!arg1 || !arg2)
		return 0;

	return (float) *arg1 >= *arg2;
}

/* ========== PRIVATE ROUTINES ========== */

/* From "fdlibm" @ netlib.att.com */

#ifndef HAVE_RINT

/* @(#)s_rint.c 5.1 93/09/24 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

/*
 * rint(x)
 * Return x rounded to integral value according to the prevailing
 * rounding mode.
 * Method:
 *		Using floating addition.
 * Exception:
 *		Inexact flag raised if x not equal to rint(x).
 */

#ifdef __STDC__
static const double
#else
static double
#endif
			one = 1.0,
			TWO52[2] = {
	4.50359962737049600000e+15, /* 0x43300000, 0x00000000 */
	-4.50359962737049600000e+15,/* 0xC3300000, 0x00000000 */
};

#ifdef __STDC__
static double
rint(double x)
#else
static double
rint(x)
double		x;

#endif
{
	int			i0,
				n0,
				j0,
				sx;
	unsigned	i,
				i1;
	double		w,
				t;

	n0 = (*((int *) &one) >> 29) ^ 1;
	i0 = *(n0 + (int *) &x);
	sx = (i0 >> 31) & 1;
	i1 = *(1 - n0 + (int *) &x);
	j0 = ((i0 >> 20) & 0x7ff) - 0x3ff;
	if (j0 < 20)
	{
		if (j0 < 0)
		{
			if (((i0 & 0x7fffffff) | i1) == 0)
				return x;
			i1 |= (i0 & 0x0fffff);
			i0 &= 0xfffe0000;
			i0 |= ((i1 | -i1) >> 12) & 0x80000;
			*(n0 + (int *) &x) = i0;
			w = TWO52[sx] + x;
			t = w - TWO52[sx];
			i0 = *(n0 + (int *) &t);
			*(n0 + (int *) &t) = (i0 & 0x7fffffff) | (sx << 31);
			return t;
		}
		else
		{
			i = (0x000fffff) >> j0;
			if (((i0 & i) | i1) == 0)
				return x;		/* x is integral */
			i >>= 1;
			if (((i0 & i) | i1) != 0)
			{
				if (j0 == 19)
					i1 = 0x40000000;
				else
					i0 = (i0 & (~i)) | ((0x20000) >> j0);
			}
		}
	}
	else if (j0 > 51)
	{
		if (j0 == 0x400)
			return x + x;		/* inf or NaN */
		else
			return x;			/* x is integral */
	}
	else
	{
		i = ((unsigned) (0xffffffff)) >> (j0 - 20);
		if ((i1 & i) == 0)
			return x;			/* x is integral */
		i >>= 1;
		if ((i1 & i) != 0)
			i1 = (i1 & (~i)) | ((0x40000000) >> (j0 - 20));
	}
	*(n0 + (int *) &x) = i0;
	*(1 - n0 + (int *) &x) = i1;
	w = TWO52[sx] + x;
	return w - TWO52[sx];
}

#endif	 /* !HAVE_RINT */

#ifndef HAVE_CBRT

static
double
cbrt(x)
double		x;
{
	int			isneg = (x < 0.0);
	double		tmpres = pow(fabs(x), (double) 1.0 / (double) 3.0);

	return isneg ? -tmpres : tmpres;
}

#endif	 /* !HAVE_CBRT */
