#include "postgres.h"

#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>				/* faked on sunos4 */
#include <math.h>
#include <limits.h>

#include "fmgr.h"
#include "utils/timestamp.h"
#include "utils/builtins.h"


int4 ascii(text *string);
text *ichar(int4 cvalue);
text *repeat(text *string, int4 count);
Interval *interval_mul(Interval *span1, float8 *arg2);
float64 dasin(float64 arg1);
float64 datan(float64 arg1);
float64 datan2(float64 arg1, float64 arg2);
float64 dcos(float64 arg1);
float64 dcot(float64 arg1);
float64 dsin(float64 arg1);
float64 dtan(float64 arg1);
float64 degrees(float64 arg1);
float64 dpi(void);
float64 radians(float64 arg1);
float64 drandom(void);
void setseed(int32 seed);


int4
ascii(text *string)
{
	if (!PointerIsValid(string))
		return 0;

	if (VARSIZE(string) <= VARHDRSZ)
		return 0;

	return ((int) *(VARDATA(string)));
} /* ascii() */


text *
ichar(int4 cvalue)
{
	text   *result;

	result = (text *) palloc(VARHDRSZ + 1);
	VARSIZE(result) = VARHDRSZ + 1;
	*VARDATA(result) = (char) cvalue;

	return result;
} /* ichar() */


text *
repeat(text *string, int4 count)
{
	text   *result;
	int		slen, tlen;
	int		i;
	char   *cp;

	if (count < 0)
		count = 0;

	slen = (VARSIZE(string)-VARHDRSZ);
	tlen = (VARHDRSZ + (count * slen));

	result = (text *) palloc(tlen);

	VARSIZE(result) = tlen;
	cp = VARDATA(result);
	for (i = 0; i < count; i++)
	{
		memcpy(cp, VARDATA(string), slen);
		cp += slen;
	}

	return result;
} /* ichar() */

Interval   *
interval_mul(Interval *span1, float8 *arg2)
{
	Interval   *result;
	double		months;

	if ((!PointerIsValid(span1)) || (!PointerIsValid(arg2)))
		return NULL;

	if (!PointerIsValid(result = palloc(sizeof(Interval))))
		elog(ERROR, "Memory allocation failed, can't divide intervals");

	months = (span1->month * *arg2);
	result->month = rint(months);
	result->time = JROUND(span1->time * *arg2);
	result->time += JROUND((months - result->month) * 30);

	return result;
}	/* interval_mul() */

/*
 *		dasin			- returns a pointer to the arcsin of arg1 (radians)
 */
float64
dasin(float64 arg1)
{
	float64		result;
	double		tmp;

	if (!PointerIsValid(arg1))
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp = *arg1;
	errno = 0;
	*result = (float64data) asin(tmp);
	if (errno != 0
#ifdef HAVE_FINITE
		|| !finite(*result)
#endif
		)
		elog(ERROR, "dasin(%f) input is out of range", *arg1);

	CheckFloat8Val(*result);
	return result;
} /* dasin() */


/*
 *		datan			- returns a pointer to the arctan of arg1 (radians)
 */
float64
datan(float64 arg1)
{
	float64		result;
	double		tmp;

	if (!PointerIsValid(arg1))
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp = *arg1;
	errno = 0;
	*result = (float64data) atan(tmp);
	if (errno != 0
#ifdef HAVE_FINITE
		|| !finite(*result)
#endif
		)
		elog(ERROR, "atan(%f) input is out of range", *arg1);

	CheckFloat8Val(*result);
	return result;
} /* datan() */


/*
 *		atan2			- returns a pointer to the arctan2 of arg1 (radians)
 */
float64
datan2(float64 arg1, float64 arg2)
{
	float64		result;

	if (!PointerIsValid(arg1) || !PointerIsValid(arg1))
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	errno = 0;
	*result = (float64data) atan2(*arg1, *arg2);
	if (errno != 0
#ifdef HAVE_FINITE
		|| !finite(*result)
#endif
		)
		elog(ERROR, "atan2(%f,%f) input is out of range", *arg1, *arg2);

	CheckFloat8Val(*result);
	return result;
} /* datan2() */


/*
 *		dcos			- returns a pointer to the cosine of arg1 (radians)
 */
float64
dcos(float64 arg1)
{
	float64		result;
	double		tmp;

	if (!PointerIsValid(arg1))
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp = *arg1;
	errno = 0;
	*result = (float64data) cos(tmp);
	if (errno != 0
#ifdef HAVE_FINITE
		|| !finite(*result)
#endif
		)
		elog(ERROR, "dcos(%f) input is out of range", *arg1);

	CheckFloat8Val(*result);
	return result;
} /* dcos() */


/*
 *		dcot			- returns a pointer to the cotangent of arg1 (radians)
 */
float64
dcot(float64 arg1)
{
	float64		result;
	double		tmp;

	if (!PointerIsValid(arg1))
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp = *arg1;
	errno = 0;
	*result = (float64data) tan(tmp);
	if ((errno != 0) || (*result == 0.0)
#ifdef HAVE_FINITE
		|| !finite(*result)
#endif
		)
		elog(ERROR, "dcot(%f) input is out of range", *arg1);

	*result = 1.0/(*result);
	CheckFloat8Val(*result);
	return result;
} /* dcot() */


/*
 *		dsin			- returns a pointer to the sine of arg1 (radians)
 */
float64
dsin(float64 arg1)
{
	float64		result;
	double		tmp;

	if (!PointerIsValid(arg1))
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp = *arg1;
	errno = 0;
	*result = (float64data) sin(tmp);
	if (errno != 0
#ifdef HAVE_FINITE
		|| !finite(*result)
#endif
		)
		elog(ERROR, "dsin(%f) input is out of range", *arg1);

	CheckFloat8Val(*result);
	return result;
} /* dsin() */


/*
 *		dtan			- returns a pointer to the tangent of arg1 (radians)
 */
float64
dtan(float64 arg1)
{
	float64		result;
	double		tmp;

	if (!PointerIsValid(arg1))
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	tmp = *arg1;
	errno = 0;
	*result = (float64data) tan(tmp);
	if (errno != 0
#ifdef HAVE_FINITE
		|| !finite(*result)
#endif
		)
		elog(ERROR, "dtan(%f) input is out of range", *arg1);

	CheckFloat8Val(*result);
	return result;
} /* dtan() */


#ifndef M_PI
/* from my RH5.2 gcc math.h file - thomas 2000-04-03 */
#define M_PI 3.14159265358979323846
#endif


/*
 *		degrees		- returns a pointer to degrees converted from radians
 */
float64
degrees(float64 arg1)
{
	float64		result;

	if (!arg1)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	*result = ((*arg1) * (180.0 / M_PI));

	CheckFloat8Val(*result);
	return result;
} /* degrees() */


/*
 *		dpi				- returns a pointer to degrees converted to radians
 */
float64
dpi(void)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));

	*result = (M_PI);

	return result;
} /* dpi() */


/*
 *		radians		- returns a pointer to radians converted from degrees
 */
float64
radians(float64 arg1)
{
	float64		result;

	if (!arg1)
		return (float64) NULL;

	result = (float64) palloc(sizeof(float64data));

	*result = ((*arg1) * (M_PI / 180.0));

	CheckFloat8Val(*result);
	return result;
} /* radians() */


#ifdef RAND_MAX

/*
 *		drandom   	- returns a random number
 */
float64
drandom(void)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));

	/* result 0.0-1.0 */
	*result = (((double)rand()) / RAND_MAX);

	CheckFloat8Val(*result);
	return result;
} /* drandom() */


/*
 *		setseed   	- set seed for the random number generator
 */
void
setseed(int32 seed)
{
	srand(seed);

	return;
} /* setseed() */

#endif


