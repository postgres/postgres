/* ----------
 * Module statmath
 *
 *	statistical aggregates for average, variance and standard
 *	deviation.
 *
 *	Jan Wieck
 * ----------
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>

#include "postgres.h"
#include "utils/palloc.h"


/* ----------
 * Declarations
 *
 * statmath_stateval_in()	Input function for state transition variable
 * statmath_stateval_out()	Output function for state transition variable
 * statmath_collect()		State transition function to collect items
 * statmath_average_fin()	Final function for average aggregate
 * statmath_variance_fin()	Final function for variance aggregate
 * statmath_stddev_fin()	Final function for deviation aggregate
 * ----------
 */

float64 statmath_stateval_in(char *str);
char *statmath_stateval_out(float64 sval);

float64 statmath_collect(float64 sval, float64 item);
float64 statmath_average_fin(float64 sval, int4 n);
float64 statmath_variance_fin(float64 sval, int4 n);
float64 statmath_stddev_fin(float64 sval, int4 n);


/* ----------
 * statmath_checkval -
 *
 *	Bounds checking for float8 values in Postgres
 * ----------
 */
static void
statmath_checkval(double val)
{
	if (fabs(val) > DBL_MAX)
		elog(ERROR, "statmath: overflow");
	if (val != 0.0 && fabs(val) < DBL_MIN)
		elog(ERROR, "statmath: underflow");
}


/* ----------
 * statmath_stateval_in -
 *
 *	Input function for the state transition value data type.
 *	The input string are two float8's separated with a colon ':'.
 * ----------
 */
float64
statmath_stateval_in(char *str)
{
	float64		retval;
	double		tmp;
	char		*cp1, *cp2;

	if (!str)
		return (float64) NULL;

	/*
	 * Allocate space for the result
	 */
	retval = (float64) palloc(sizeof(float64data) * 2);

	/*
	 * Get the first number
	 */
	errno = 0;
	tmp = strtod(str, &cp1);
	if (*cp1 != ':' || errno == ERANGE)
		elog(ERROR, "statmath: illegal input format '%s'", str);
	statmath_checkval(tmp);
	retval[0] = tmp;

	/*
	 * Get the second number
	 */
	tmp = strtod(++cp1, &cp2);
	if (*cp2 != '\0' || errno == ERANGE)
		elog(ERROR, "statmath: illegal input format '%s'", str);
	statmath_checkval(tmp);
	retval[1] = tmp;

	/*
	 * Return the internal binary format
	 */
	return retval;
}


/* ----------
 * statmath_stateval_out -
 *
 *	Output function for the state transition value data type.
 * ----------
 */
char *
statmath_stateval_out(float64 sval)
{
	char		buf[1024];
	double		v1, v2;

	if (!sval)
		return pstrdup("(null)");
	
	/*
	 * Print the values in the external format and return
	 * the result in allocated space
	 */
	v1 = sval[0];
	v2 = sval[1];
	sprintf(buf, "%.*g:%.*g", DBL_DIG, v1, DBL_DIG, v2);
	return pstrdup(buf);
}


/* ----------
 * statmath_collect -
 *
 *	State transition function to collect data for the variance
 *	and standard deviation aggregates.
 *	The state transition variable holds 2 float8 values. The
 *	first is the sum of the items, the second the sum of the
 *	item quadratic products.
 * ----------
 */
float64
statmath_collect(float64 sval, float64 item)
{
	float64		retval;
	double		tmp;

	if (!sval || !item)
		return (float64) NULL;

	/*
	 * Allocate space for the result
	 */
	retval = (float64) palloc(sizeof(float64data) * 2);

	/*
	 * Compute the new values
	 */
	tmp = sval[0] + *item;
	statmath_checkval(tmp);
	retval[0] = tmp;

	tmp = sval[1] + *item * *item;
	statmath_checkval(tmp);
	retval[1] = tmp;

	/*
	 * Return the result
	 */
	return retval;
}


/* ----------
 * statmath_average_fin -
 *
 *	Final computation function for the average aggregate.
 * ----------
 */
float64
statmath_average_fin(float64 sum, int4 n)
{
	float64		retval;
	double		tmp;

	if (!sum)
		return (float64) NULL;

	/*
	 * Allocate space for the result
	 */
	retval = (float64) palloc(sizeof(float64data));

	/*
	 * Avoid division by zero if no items collected
	 */
	if (n == 0)
	{
		*retval = 0.0;
		return retval;
	}

	/*
	 * Compute the average
	 */
	tmp = *sum / (double)n;
	statmath_checkval(tmp);
	*retval = tmp;

	/*
	 * Return the result
	 */
	return retval;
}


/* ----------
 * statmath_variance_fin -
 *
 *	Final computation function for the variance aggregate
 * ----------
 */
float64
statmath_variance_fin(float64 sval, int4 n)
{
	float64		retval;
	double		avg;
	double		variance;

	if (!sval)
		return (float64) NULL;

	/*
	 * Allocate space for the result
	 */
	retval = (float64) palloc(sizeof(float64data));

	/*
	 * Avoid division by zero if less than 2 items collected
	 */
	if (n < 2)
	{
		*retval = 0.0;
		return retval;
	}

	/*
	 * Calculate the variance
	 */
	avg = sval[0] / (double)n;
	variance = (sval[1] - sval[0] * avg) / ((double)n - 1.0);

	statmath_checkval(variance);
	*retval = variance;

	/*
	 * Return the result
	 */
	return retval;
}


/* ----------
 * statmath_stateval_in -
 *
 *	Input function for the state transition value data type
 * ----------
 */
float64
statmath_stddev_fin(float64 sval, int4 n)
{
	float64		retval;
	double		avg;
	double		stddev;

	if (!sval)
		return (float64) NULL;

	/*
	 * Allocate space for the result
	 */
	retval = (float64) palloc(sizeof(float64data));

	/*
	 * Avoid division by zero if less than 2 items collected
	 */
	if (n < 2)
	{
		*retval = 0.0;
		return retval;
	}

	/*
	 * Calculate the standard deviation
	 */
	avg = sval[0] / (double)n;
	stddev = sqrt((sval[1] - sval[0] * avg) / ((double)n - 1.0));

	statmath_checkval(stddev);
	*retval = stddev;

	/*
	 * Return the result
	 */
	return retval;
}


