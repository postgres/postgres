/*-------------------------------------------------------------------------
 *
 * int8.c
 *	  Internal 64-bit integer operations
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <float.h>

#include "postgres.h"

#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif

#include "utils/int8.h"

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
int64 *
int8in(char *str)
{
	int64	   *result = palloc(sizeof(int64));
	char	   *ptr = str;
	int64		tmp = 0;
	int			sign = 1;

	if (!PointerIsValid(str))
		elog(ERROR, "Bad (null) int8 external representation");

	/*
	 * Do our own scan, rather than relying on sscanf which might be
	 * broken for long long.
	 */
	while (*ptr && isspace(*ptr))		/* skip leading spaces */
		ptr++;
	if (*ptr == '-')			/* handle sign */
		sign = -1, ptr++;
	else if (*ptr == '+')
		ptr++;
	if (!isdigit(*ptr))			/* require at least one digit */
		elog(ERROR, "Bad int8 external representation \"%s\"", str);
	while (*ptr && isdigit(*ptr))		/* process digits */
	{
		int64		newtmp = tmp * 10 + (*ptr++ - '0');

		if ((newtmp / 10) != tmp)		/* overflow? */
			elog(ERROR, "int8 value out of range: \"%s\"", str);
		tmp = newtmp;
	}
	if (*ptr)					/* trailing junk? */
		elog(ERROR, "Bad int8 external representation \"%s\"", str);

	*result = (sign < 0) ? -tmp : tmp;

	return result;
}	/* int8in() */


/* int8out()
 */
char *
int8out(int64 *val)
{
	char	   *result;

	int			len;
	char		buf[MAXINT8LEN + 1];

	if (!PointerIsValid(val))
		return NULL;

	if ((len = snprintf(buf, MAXINT8LEN, INT64_FORMAT, *val)) < 0)
		elog(ERROR, "Unable to format int8");

	result = palloc(len + 1);

	strcpy(result, buf);

	return result;
}	/* int8out() */


/*----------------------------------------------------------
 *	Relational operators for int8s.
 *---------------------------------------------------------*/

/* int8relop()
 * Is val1 relop val2?
 */
bool
int8eq(int64 *val1, int64 *val2)
{
	if (!val1 || !val2)
		return 0;

	return *val1 == *val2;
}	/* int8eq() */

bool
int8ne(int64 *val1, int64 *val2)
{
	if (!val1 || !val2)
		return 0;

	return *val1 != *val2;
}	/* int8ne() */

bool
int8lt(int64 *val1, int64 *val2)
{
	if (!val1 || !val2)
		return 0;

	return *val1 < *val2;
}	/* int8lt() */

bool
int8gt(int64 *val1, int64 *val2)
{
	if (!val1 || !val2)
		return 0;

	return *val1 > *val2;
}	/* int8gt() */

bool
int8le(int64 *val1, int64 *val2)
{
	if (!val1 || !val2)
		return 0;

	return *val1 <= *val2;
}	/* int8le() */

bool
int8ge(int64 *val1, int64 *val2)
{
	if (!val1 || !val2)
		return 0;

	return *val1 >= *val2;
}	/* int8ge() */


/* int84relop()
 * Is 64-bit val1 relop 32-bit val2?
 */
bool
int84eq(int64 *val1, int32 val2)
{
	if (!val1)
		return 0;

	return *val1 == val2;
}	/* int84eq() */

bool
int84ne(int64 *val1, int32 val2)
{
	if (!val1)
		return 0;

	return *val1 != val2;
}	/* int84ne() */

bool
int84lt(int64 *val1, int32 val2)
{
	if (!val1)
		return 0;

	return *val1 < val2;
}	/* int84lt() */

bool
int84gt(int64 *val1, int32 val2)
{
	if (!val1)
		return 0;

	return *val1 > val2;
}	/* int84gt() */

bool
int84le(int64 *val1, int32 val2)
{
	if (!val1)
		return 0;

	return *val1 <= val2;
}	/* int84le() */

bool
int84ge(int64 *val1, int32 val2)
{
	if (!val1)
		return 0;

	return *val1 >= val2;
}	/* int84ge() */


/* int48relop()
 * Is 32-bit val1 relop 64-bit val2?
 */
bool
int48eq(int32 val1, int64 *val2)
{
	if (!val2)
		return 0;

	return val1 == *val2;
}	/* int48eq() */

bool
int48ne(int32 val1, int64 *val2)
{
	if (!val2)
		return 0;

	return val1 != *val2;
}	/* int48ne() */

bool
int48lt(int32 val1, int64 *val2)
{
	if (!val2)
		return 0;

	return val1 < *val2;
}	/* int48lt() */

bool
int48gt(int32 val1, int64 *val2)
{
	if (!val2)
		return 0;

	return val1 > *val2;
}	/* int48gt() */

bool
int48le(int32 val1, int64 *val2)
{
	if (!val2)
		return 0;

	return val1 <= *val2;
}	/* int48le() */

bool
int48ge(int32 val1, int64 *val2)
{
	if (!val2)
		return 0;

	return val1 >= *val2;
}	/* int48ge() */


/*----------------------------------------------------------
 *	Arithmetic operators on 64-bit integers.
 *---------------------------------------------------------*/

int64 *
int8um(int64 *val)
{
	int64		temp = 0;
	int64	   *result = palloc(sizeof(int64));

	if (!PointerIsValid(val))
		return NULL;

	result = int8mi(&temp, val);

	return result;
}	/* int8um() */


int64 *
int8pl(int64 *val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if ((!PointerIsValid(val1)) || (!PointerIsValid(val2)))
		return NULL;

	*result = *val1 + *val2;

	return result;
}	/* int8pl() */

int64 *
int8mi(int64 *val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if ((!PointerIsValid(val1)) || (!PointerIsValid(val2)))
		return NULL;

	*result = *val1 - *val2;

	return result;
}	/* int8mi() */

int64 *
int8mul(int64 *val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if ((!PointerIsValid(val1)) || (!PointerIsValid(val2)))
		return NULL;

	*result = *val1 * *val2;

	return result;
}	/* int8mul() */

int64 *
int8div(int64 *val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if ((!PointerIsValid(val1)) || (!PointerIsValid(val2)))
		return NULL;

	*result = *val1 / *val2;

	return result;
}	/* int8div() */

/* int8abs()
 * Absolute value
 */
int64 *
int8abs(int64 *arg1)
{
	int64	   *result;

	if (!PointerIsValid(arg1))
		return NULL;

	result = palloc(sizeof(*result));

	*result = ((*arg1 < 0) ? -*arg1 : *arg1);

	return result;
}

/* int8mod()
 * Modulo operation.
 */
int64 *
int8mod(int64 *val1, int64 *val2)
{
	int64	   *result;

	/* use the divide operation to check params and allocate storage */
	result = int8div(val1, val2);
	*result *= *val2;
	*result = *val1 - *result;

	return result;
}	/* int8mod() */

/* int8fac()
 * Factorial
 */
int64 *
int8fac(int64 *arg1)
{
	int64	   *result;
	int64		i;

	if (!PointerIsValid(arg1))
		return NULL;

	result = palloc(sizeof(*result));

	if (*arg1 < 1)
		*result = 0;
	else
		for (i = *arg1, *result = 1; i > 0; --i)
			*result *= i;

	return result;
}

int64 *
int8larger(int64 *val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if ((!PointerIsValid(val1)) || (!PointerIsValid(val2)))
		return NULL;

	*result = ((*val1 > *val2) ? *val1 : *val2);

	return result;
}	/* int8larger() */

int64 *
int8smaller(int64 *val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if ((!PointerIsValid(val1)) || (!PointerIsValid(val2)))
		return NULL;

	*result = ((*val1 < *val2) ? *val1 : *val2);

	return result;
}	/* int8smaller() */


int64 *
int84pl(int64 *val1, int32 val2)
{
	int64	   *result = palloc(sizeof(int64));

	if (!PointerIsValid(val1))
		return NULL;

	*result = *val1 + (int64) val2;

	return result;
}	/* int84pl() */

int64 *
int84mi(int64 *val1, int32 val2)
{
	int64	   *result = palloc(sizeof(int64));

	if (!PointerIsValid(val1))
		return NULL;

	*result = *val1 - (int64) val2;

	return result;
}	/* int84mi() */

int64 *
int84mul(int64 *val1, int32 val2)
{
	int64	   *result = palloc(sizeof(int64));

	if (!PointerIsValid(val1))
		return NULL;

	*result = *val1 * (int64) val2;

	return result;
}	/* int84mul() */

int64 *
int84div(int64 *val1, int32 val2)
{
	int64	   *result = palloc(sizeof(int64));

	if (!PointerIsValid(val1))
		return NULL;

	*result = *val1 / (int64) val2;

	return result;
}	/* int84div() */


int64 *
int48pl(int32 val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if (!PointerIsValid(val2))
		return NULL;

	*result = (int64) val1 + *val2;

	return result;
}	/* int48pl() */

int64 *
int48mi(int32 val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if (!PointerIsValid(val2))
		return NULL;

	*result = (int64) val1 - *val2;

	return result;
}	/* int48mi() */

int64 *
int48mul(int32 val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if (!PointerIsValid(val2))
		return NULL;

	*result = (int64) val1 **val2;

	return result;
}	/* int48mul() */

int64 *
int48div(int32 val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if (!PointerIsValid(val2))
		return NULL;

	*result = (int64) val1 / *val2;

	return result;
}	/* int48div() */


/*----------------------------------------------------------
 *	Conversion operators.
 *---------------------------------------------------------*/

int64 *
int48(int32 val)
{
	int64	   *result = palloc(sizeof(int64));

	*result = val;

	return result;
}	/* int48() */

int32
int84(int64 *val)
{
	int32		result;

	if (!PointerIsValid(val))
		elog(ERROR, "Invalid (null) int64, can't convert int8 to int4");

	if ((*val < INT_MIN) || (*val > INT_MAX))
		elog(ERROR, "int8 conversion to int4 is out of range");

	result = *val;

	return result;
}	/* int84() */

#if NOT_USED
int64 *
int2vector	(int16 val)
{
	int64	   *result;

	result = palloc(sizeof(int64));

	*result = val;

	return result;
}	/* int2vector() */

int16
int82(int64 *val)
{
	int16		result;

	if (!PointerIsValid(val))
		elog(ERROR, "Invalid (null) int8, can't convert to int2");

	if ((*val < SHRT_MIN) || (*val > SHRT_MAX))
		elog(ERROR, "int8 conversion to int2 is out of range");

	result = *val;

	return result;
}	/* int82() */

#endif

float64
i8tod(int64 *val)
{
	float64		result = palloc(sizeof(float64data));

	if (!PointerIsValid(val))
		elog(ERROR, "Invalid (null) int8, can't convert to float8");

	*result = *val;

	return result;
}	/* i8tod() */

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
int64 *
dtoi8(float64 val)
{
	int64	   *result = palloc(sizeof(int64));

	if (!PointerIsValid(val))
		elog(ERROR, "Invalid (null) float8, can't convert to int8");

	if ((*val < (-pow(2, 63) + 1)) || (*val > (pow(2, 63) - 1)))
		elog(ERROR, "Floating point conversion to int64 is out of range");

	*result = *val;

	return result;
}	/* dtoi8() */

/* text_int8()
 */
int64 *
text_int8(text *str)
{
	int			len;
	char	   *s;

	if (!PointerIsValid(str))
		elog(ERROR, "Bad (null) int8 external representation");

	len = (VARSIZE(str) - VARHDRSZ);
	s = palloc(len + 1);
	memmove(s, VARDATA(str), len);
	*(s + len) = '\0';

	return int8in(s);
}	/* text_int8() */


/* int8_text()
 */
text *
int8_text(int64 *val)
{
	text	   *result;

	int			len;
	char	   *s;

	if (!PointerIsValid(val))
		return NULL;

	s = int8out(val);
	len = strlen(s);

	result = palloc(VARHDRSZ + len);

	VARSIZE(result) = len + VARHDRSZ;
	memmove(VARDATA(result), s, len);

	return result;
}	/* int8_text() */
