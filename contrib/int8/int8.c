/*-------------------------------------------------------------------------
 *
 * int8.c--
 *	  Internal 64-bit integer operations
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>				/* for sprintf proto, etc. */
#include <stdlib.h>				/* for strtod, etc. */
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <float.h>
#include <limits.h>

#include "postgres.h"
#include "utils/palloc.h"

#define MAXINT8LEN		25

#if defined(__alpha) || defined(__GNUC__)
#define HAVE_64BIT_INTS 1
#endif

#ifndef HAVE_64BIT_INTS
typedef char[8]
int64;

#elif defined(__alpha)
typedef long int int64;

#define INT64_FORMAT "%ld"

#elif defined(__GNUC__)
typedef long long int int64;

#define INT64_FORMAT "%Ld"

#else
typedef long int int64;

#define INT64_FORMAT "%ld"
#endif

int64	   *int8in(char *str);
char	   *int8out(int64 *val);

bool		int8eq(int64 *val1, int64 *val2);
bool		int8ne(int64 *val1, int64 *val2);
bool		int8lt(int64 *val1, int64 *val2);
bool		int8gt(int64 *val1, int64 *val2);
bool		int8le(int64 *val1, int64 *val2);
bool		int8ge(int64 *val1, int64 *val2);

bool		int84eq(int64 *val1, int32 val2);
bool		int84ne(int64 *val1, int32 val2);
bool		int84lt(int64 *val1, int32 val2);
bool		int84gt(int64 *val1, int32 val2);
bool		int84le(int64 *val1, int32 val2);
bool		int84ge(int64 *val1, int32 val2);

int64	   *int8um(int64 *val);
int64	   *int8pl(int64 *val1, int64 *val2);
int64	   *int8mi(int64 *val1, int64 *val2);
int64	   *int8mul(int64 *val1, int64 *val2);
int64	   *int8div(int64 *val1, int64 *val2);

int64	   *int48(int32 val);
int32		int84(int64 *val);

#if FALSE
int64	   *int28 (int16 val);
int16		int82(int64 *val);

#endif

float64		i8tod(int64 *val);
int64	   *dtoi8(float64 val);

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

#if HAVE_64BIT_INTS
	if (!PointerIsValid(str))
		elog(ERROR, "Bad (null) int8 external representation", NULL);

	if (sscanf(str, INT64_FORMAT, result) != 1)
		elog(ERROR, "Bad int8 external representation '%s'", str);

#else
	elog(ERROR, "64-bit integers are not supported", NULL);
	result = NULL;
#endif

	return (result);
}	/* int8in() */


/* int8out()
 */
char *
int8out(int64 *val)
{
	char	   *result;

	int			len;
	char		buf[MAXINT8LEN + 1];

#if HAVE_64BIT_INTS
	if (!PointerIsValid(val))
		return (NULL);

	if ((len = snprintf(buf, MAXINT8LEN, INT64_FORMAT, *val)) < 0)
		elog(ERROR, "Unable to format int8", NULL);

	result = palloc(len + 1);

	strcpy(result, buf);

#else
	elog(ERROR, "64-bit integers are not supported", NULL);
	result = NULL;
#endif

	return (result);
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
	return (*val1 == *val2);
}	/* int8eq() */

bool
int8ne(int64 *val1, int64 *val2)
{
	return (*val1 != *val2);
}	/* int8ne() */

bool
int8lt(int64 *val1, int64 *val2)
{
	return (*val1 < *val2);
}	/* int8lt() */

bool
int8gt(int64 *val1, int64 *val2)
{
	return (*val1 > *val2);
}	/* int8gt() */

bool
int8le(int64 *val1, int64 *val2)
{
	return (*val1 <= *val2);
}	/* int8le() */

bool
int8ge(int64 *val1, int64 *val2)
{
	return (*val1 >= *val2);
}	/* int8ge() */


/* int84relop()
 * Is 64-bit val1 relop 32-bit val2?
 */
bool
int84eq(int64 *val1, int32 val2)
{
	return (*val1 == val2);
}	/* int84eq() */

bool
int84ne(int64 *val1, int32 val2)
{
	return (*val1 != val2);
}	/* int84ne() */

bool
int84lt(int64 *val1, int32 val2)
{
	return (*val1 < val2);
}	/* int84lt() */

bool
int84gt(int64 *val1, int32 val2)
{
	return (*val1 > val2);
}	/* int84gt() */

bool
int84le(int64 *val1, int32 val2)
{
	return (*val1 <= val2);
}	/* int84le() */

bool
int84ge(int64 *val1, int32 val2)
{
	return (*val1 >= val2);
}	/* int84ge() */


/*----------------------------------------------------------
 *	Arithmetic operators on 64-bit integers.
 *---------------------------------------------------------*/

int64 *
int8um(int64 *val)
{
	int64	   *result = palloc(sizeof(int64));

	if (!PointerIsValid(val))
		return NULL;

	*result = (-*val);

	return (result);
}	/* int8um() */

int64 *
int8pl(int64 *val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if ((!PointerIsValid(val1)) || (!PointerIsValid(val2)))
		return NULL;

	*result = *val1 + *val2;

	return (result);
}	/* int8pl() */

int64 *
int8mi(int64 *val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if ((!PointerIsValid(val1)) || (!PointerIsValid(val2)))
		return NULL;

	*result = *val1 - *val2;

	return (result);
}	/* int8mi() */

int64 *
int8mul(int64 *val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if ((!PointerIsValid(val1)) || (!PointerIsValid(val2)))
		return NULL;

	*result = *val1 * *val2;

	return (result);
}	/* int8mul() */

int64 *
int8div(int64 *val1, int64 *val2)
{
	int64	   *result = palloc(sizeof(int64));

	if ((!PointerIsValid(val1)) || (!PointerIsValid(val2)))
		return NULL;

	*result = *val1 / *val2;

	return (result);
}	/* int8div() */


/*----------------------------------------------------------
 *	Conversion operators.
 *---------------------------------------------------------*/

int64 *
int48(int32 val)
{
	int64	   *result = palloc(sizeof(int64));

	*result = val;

	return (result);
}	/* int48() */

int32
int84(int64 *val)
{
	int32		result;

	if (!PointerIsValid(val))
		elog(ERROR, "Invalid (null) int64, can't convert int8 to int4", NULL);

	if ((*val < INT_MIN) || (*val > INT_MAX))
		elog(ERROR, "int8 conversion to int4 is out of range", NULL);

	result = *val;

	return (result);
}	/* int84() */

#if FALSE
int64 *
int28		(int16 val)
{
	int64	   *result;

	if (!PointerIsValid(result = palloc(sizeof(int64))))
		elog(ERROR, "Memory allocation failed, can't convert int8 to int2", NULL);

	*result = val;

	return (result);
}	/* int28() */

int16
int82(int64 *val)
{
	int16		result;

	if (!PointerIsValid(val))
		elog(ERROR, "Invalid (null) int8, can't convert to int2", NULL);

	result = *val;

	return (result);
}	/* int82() */

#endif

float64
i8tod(int64 *val)
{
	float64		result = palloc(sizeof(float64data));

	*result = *val;

	return (result);
}	/* i8tod() */

int64 *
dtoi8(float64 val)
{
	int64	   *result = palloc(sizeof(int64));

	if ((*val < (-pow(2, 64) + 1)) || (*val > (pow(2, 64) - 1)))
		elog(ERROR, "Floating point conversion to int64 is out of range", NULL);

	*result = *val;

	return (result);
}	/* dtoi8() */
