/*-------------------------------------------------------------------------
 *
 * date.c
 *	  implements DATE and TIME data types specified in SQL-92 standard
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/date.c,v 1.46 2000/06/15 03:32:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <limits.h>
#include <time.h>

#include "postgres.h"
#ifdef HAVE_FLOAT_H
#include <float.h>
#endif
#include "miscadmin.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/nabstime.h"
#include "utils/builtins.h"

static int
	date2tm(DateADT dateVal, int *tzp, struct tm * tm, double *fsec, char **tzn);


/*****************************************************************************
 *	 Date ADT
 *****************************************************************************/


/* date_in()
 * Given date text string, convert to internal date format.
 */
Datum
date_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	DateADT		date;
	double		fsec;
	struct tm	tt,
			   *tm = &tt;
	int			tzp;
	int			dtype;
	int			nf;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + 1];

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
		|| (DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tzp) != 0))
		elog(ERROR, "Bad date external representation '%s'", str);

	switch (dtype)
	{
		case DTK_DATE:
			break;

		case DTK_CURRENT:
			GetCurrentTime(tm);
			break;

		case DTK_EPOCH:
			tm->tm_year = 1970;
			tm->tm_mon = 1;
			tm->tm_mday = 1;
			break;

		default:
			elog(ERROR, "Unrecognized date external representation '%s'", str);
	}

	date = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000, 1, 1));

	PG_RETURN_DATEADT(date);
}

/* date_out()
 * Given internal format date, convert to text string.
 */
Datum
date_out(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);
	char	   *result;
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];

	j2date((date + date2j(2000, 1, 1)),
		   &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));

	EncodeDateOnly(tm, DateStyle, buf);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

Datum
date_eq(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 == dateVal2);
}

Datum
date_ne(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 != dateVal2);
}

Datum
date_lt(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 < dateVal2);
}

Datum
date_le(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 <= dateVal2);
}

Datum
date_gt(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 > dateVal2);
}

Datum
date_ge(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 >= dateVal2);
}

Datum
date_cmp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	if (dateVal1 < dateVal2)
		PG_RETURN_INT32(-1);
	else if (dateVal1 > dateVal2)
		PG_RETURN_INT32(1);
	PG_RETURN_INT32(0);
}

Datum
date_larger(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_DATEADT((dateVal1 > dateVal2) ? dateVal1 : dateVal2);
}

Datum
date_smaller(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_DATEADT((dateVal1 < dateVal2) ? dateVal1 : dateVal2);
}

/* Compute difference between two dates in days.
 */
Datum
date_mi(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_INT32((int32) (dateVal1 - dateVal2));
}

/* Add a number of days to a date, giving a new date.
 * Must handle both positive and negative numbers of days.
 */
Datum
date_pli(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	int32		days = PG_GETARG_INT32(1);

	PG_RETURN_DATEADT(dateVal + days);
}

/* Subtract a number of days from a date, giving a new date.
 */
Datum
date_mii(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	int32		days = PG_GETARG_INT32(1);

	PG_RETURN_DATEADT(dateVal - days);
}

/* date_timestamp()
 * Convert date to timestamp data type.
 */
Datum
date_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Timestamp	result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	double		fsec = 0;
	char	   *tzn;

	if (date2tm(dateVal, &tz, tm, &fsec, &tzn) != 0)
		elog(ERROR, "Unable to convert date to timestamp");

	if (tm2timestamp(tm, fsec, &tz, &result) != 0)
		elog(ERROR, "Timestamp out of range");

	PG_RETURN_TIMESTAMP(result);
}


/* timestamp_date()
 * Convert timestamp to date data type.
 */
Datum
timestamp_date(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	DateADT		result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	double		fsec;
	char	   *tzn;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		elog(ERROR, "Unable to convert timestamp to date");

	if (TIMESTAMP_IS_EPOCH(timestamp))
	{
		timestamp2tm(SetTimestamp(timestamp), NULL, tm, &fsec, NULL);
	}
	else if (TIMESTAMP_IS_CURRENT(timestamp))
	{
		timestamp2tm(SetTimestamp(timestamp), &tz, tm, &fsec, &tzn);
	}
	else
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, &tzn) != 0)
			elog(ERROR, "Unable to convert timestamp to date");
	}

	result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000, 1, 1);

	PG_RETURN_DATEADT(result);
}


/* abstime_date()
 * Convert abstime to date data type.
 */
Datum
abstime_date(PG_FUNCTION_ARGS)
{
	AbsoluteTime abstime = PG_GETARG_ABSOLUTETIME(0);
	DateADT		result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;

	switch (abstime)
	{
		case INVALID_ABSTIME:
		case NOSTART_ABSTIME:
		case NOEND_ABSTIME:
			elog(ERROR, "Unable to convert reserved abstime value to date");

			/*
			 * pretend to drop through to make compiler think that result
			 * will be set
			 */

		case EPOCH_ABSTIME:
			result = date2j(1970, 1, 1) - date2j(2000, 1, 1);
			break;

		case CURRENT_ABSTIME:
			GetCurrentTime(tm);
			result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000, 1, 1);
			break;

		default:
			abstime2tm(abstime, &tz, tm, NULL);
			result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000, 1, 1);
			break;
	}

	PG_RETURN_DATEADT(result);
}


/* date2tm()
 * Convert date to time structure.
 * Note that date is an implicit local time, but the system calls assume
 *	that everything is GMT. So, convert to GMT, rotate to local time,
 *	and then convert again to try to get the time zones correct.
 */
static int
date2tm(DateADT dateVal, int *tzp, struct tm * tm, double *fsec, char **tzn)
{
	struct tm  *tx;
	time_t		utime;

	*fsec = 0;

	j2date((dateVal + date2j(2000, 1, 1)), &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	tm->tm_isdst = -1;

	if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
	{

		/* convert to system time */
		utime = ((dateVal + (date2j(2000, 1, 1) - date2j(1970, 1, 1))) * 86400);
		/* rotate to noon to get the right day in time zone */
		utime += (12 * 60 * 60);

#ifdef USE_POSIX_TIME
		tx = localtime(&utime);

		tm->tm_year = tx->tm_year + 1900;
		tm->tm_mon = tx->tm_mon + 1;
		tm->tm_mday = tx->tm_mday;
		tm->tm_isdst = tx->tm_isdst;

#if defined(HAVE_TM_ZONE)
		tm->tm_gmtoff = tx->tm_gmtoff;
		tm->tm_zone = tx->tm_zone;

		/* tm_gmtoff is Sun/DEC-ism */
		*tzp = -(tm->tm_gmtoff);
		if (tzn != NULL)
			*tzn = (char *) tm->tm_zone;
#elif defined(HAVE_INT_TIMEZONE)
#ifdef __CYGWIN__
		*tzp = (tm->tm_isdst ? (_timezone - 3600) : _timezone);
#else
		*tzp = (tm->tm_isdst ? (timezone - 3600) : timezone);
#endif
		if (tzn != NULL)
			*tzn = tzname[(tm->tm_isdst > 0)];
#else
#error USE_POSIX_TIME is defined but neither HAVE_TM_ZONE or HAVE_INT_TIMEZONE are defined
#endif
#else							/* !USE_POSIX_TIME */
		*tzp = CTimeZone;		/* V7 conventions; don't know timezone? */
		if (tzn != NULL)
			*tzn = CTZName;
#endif

		/* otherwise, outside of timezone range so convert to GMT... */
	}
	else
	{
		*tzp = 0;
		tm->tm_isdst = 0;
		if (tzn != NULL)
			*tzn = NULL;
	}

	return 0;
}	/* date2tm() */


/*****************************************************************************
 *	 Time ADT
 *****************************************************************************/

Datum
time_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	TimeADT		time;
	double		fsec;
	struct tm	tt,
			   *tm = &tt;
	int			nf;
	char		lowstr[MAXDATELEN + 1];
	char	   *field[MAXDATEFIELDS];
	int			dtype;
	int			ftype[MAXDATEFIELDS];

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
	 || (DecodeTimeOnly(field, ftype, nf, &dtype, tm, &fsec, NULL) != 0))
		elog(ERROR, "Bad time external representation '%s'", str);

	time = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec + fsec);

	PG_RETURN_TIMEADT(time);
}

Datum
time_out(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	char	   *result;
	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	char		buf[MAXDATELEN + 1];

	tm->tm_hour = (time / (60 * 60));
	tm->tm_min = (((int) (time / 60)) % 60);
	tm->tm_sec = (((int) time) % 60);

	fsec = 0;

	EncodeTimeOnly(tm, fsec, NULL, DateStyle, buf);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}


Datum
time_eq(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 == time2);
}

Datum
time_ne(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 != time2);
}

Datum
time_lt(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 < time2);
}

Datum
time_le(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 <= time2);
}

Datum
time_gt(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 > time2);
}

Datum
time_ge(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 >= time2);
}

Datum
time_cmp(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	if (time1 < time2)
		PG_RETURN_INT32(-1);
	if (time1 > time2)
		PG_RETURN_INT32(1);
	PG_RETURN_INT32(0);
}

Datum
time_larger(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_TIMEADT((time1 > time2) ? time1 : time2);
}

Datum
time_smaller(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_TIMEADT((time1 < time2) ? time1 : time2);
}

/* overlaps_time()
 * Implements the SQL92 OVERLAPS operator.
 * Algorithm from Date and Darwen, 1997
 */
Datum
overlaps_time(PG_FUNCTION_ARGS)
{
	TimeADT		ts1 = PG_GETARG_TIMEADT(0);
	TimeADT		te1 = PG_GETARG_TIMEADT(1);
	TimeADT		ts2 = PG_GETARG_TIMEADT(2);
	TimeADT		te2 = PG_GETARG_TIMEADT(3);

	/* Make sure we have ordered pairs... */
	if (ts1 > te1)
	{
		TimeADT		tt = ts1;

		ts1 = te1;
		te1 = tt;
	}
	if (ts2 > te2)
	{
		TimeADT		tt = ts2;

		ts2 = te2;
		te2 = tt;
	}

	PG_RETURN_BOOL((ts1 > ts2 && (ts1 < te2 || te1 < te2)) ||
				   (ts1 < ts2 && (ts2 < te1 || te2 < te1)) ||
				   (ts1 == ts2));
}

/* timestamp_time()
 * Convert timestamp to time data type.
 */
Datum
timestamp_time(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	TimeADT		result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	double		fsec;
	char	   *tzn;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		elog(ERROR, "Unable to convert timestamp to date");

	if (TIMESTAMP_IS_EPOCH(timestamp))
		timestamp2tm(SetTimestamp(timestamp), NULL, tm, &fsec, NULL);
	else if (TIMESTAMP_IS_CURRENT(timestamp))
		timestamp2tm(SetTimestamp(timestamp), &tz, tm, &fsec, &tzn);
	else
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, &tzn) != 0)
			elog(ERROR, "Unable to convert timestamp to date");
	}

	result = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec + fsec);

	PG_RETURN_TIMEADT(result);
}


/* datetime_timestamp()
 * Convert date and time to timestamp data type.
 */
Datum
datetime_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);
	TimeADT		time = PG_GETARG_TIMEADT(1);
	Timestamp	result;

	result = DatumGetTimestamp(DirectFunctionCall1(date_timestamp,
												   DateADTGetDatum(date)));
	result += time;

	PG_RETURN_TIMESTAMP(result);
}


/* time_interval()
 * Convert time to interval data type.
 */
Datum
time_interval(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->time = time;
	result->month = 0;

	PG_RETURN_INTERVAL_P(result);
}


/*****************************************************************************
 *	 Time With Time Zone ADT
 *****************************************************************************/


Datum
timetz_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	TimeTzADT  *time;
	double		fsec;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	int			nf;
	char		lowstr[MAXDATELEN + 1];
	char	   *field[MAXDATEFIELDS];
	int			dtype;
	int			ftype[MAXDATEFIELDS];

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
	  || (DecodeTimeOnly(field, ftype, nf, &dtype, tm, &fsec, &tz) != 0))
		elog(ERROR, "Bad time external representation '%s'", str);

	time = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	time->time = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec + fsec);
	time->zone = tz;

	PG_RETURN_TIMETZADT_P(time);
}

Datum
timetz_out(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(0);
	char	   *result;
	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	int			tz;
	char		buf[MAXDATELEN + 1];

	tm->tm_hour = (time->time / (60 * 60));
	tm->tm_min = (((int) (time->time / 60)) % 60);
	tm->tm_sec = (((int) time->time) % 60);

	fsec = 0;
	tz = time->zone;

	EncodeTimeOnly(tm, fsec, &tz, DateStyle, buf);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}


Datum
timetz_eq(PG_FUNCTION_ARGS)
{
	TimeTzADT   *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT   *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(((time1->time+time1->zone) == (time2->time+time2->zone)));
}

Datum
timetz_ne(PG_FUNCTION_ARGS)
{
	TimeTzADT   *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT   *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(((time1->time+time1->zone) != (time2->time+time2->zone)));
}

Datum
timetz_lt(PG_FUNCTION_ARGS)
{
	TimeTzADT   *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT   *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(((time1->time+time1->zone) < (time2->time+time2->zone)));
}

Datum
timetz_le(PG_FUNCTION_ARGS)
{
	TimeTzADT   *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT   *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(((time1->time+time1->zone) <= (time2->time+time2->zone)));
}

Datum
timetz_gt(PG_FUNCTION_ARGS)
{
	TimeTzADT   *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT   *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(((time1->time+time1->zone) > (time2->time+time2->zone)));
}

Datum
timetz_ge(PG_FUNCTION_ARGS)
{
	TimeTzADT   *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT   *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(((time1->time+time1->zone) >= (time2->time+time2->zone)));
}

Datum
timetz_cmp(PG_FUNCTION_ARGS)
{
	TimeTzADT   *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT   *time2 = PG_GETARG_TIMETZADT_P(1);

	if (DatumGetBool(DirectFunctionCall2(timetz_lt,
										 TimeTzADTPGetDatum(time1),
										 TimeTzADTPGetDatum(time2))))
		PG_RETURN_INT32(-1);
	if (DatumGetBool(DirectFunctionCall2(timetz_gt,
										 TimeTzADTPGetDatum(time1),
										 TimeTzADTPGetDatum(time2))))
		PG_RETURN_INT32(1);
	PG_RETURN_INT32(0);
}

Datum
timetz_larger(PG_FUNCTION_ARGS)
{
	TimeTzADT   *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT   *time2 = PG_GETARG_TIMETZADT_P(1);

	if (DatumGetBool(DirectFunctionCall2(timetz_gt,
										 TimeTzADTPGetDatum(time1),
										 TimeTzADTPGetDatum(time2))))
		PG_RETURN_TIMETZADT_P(time1);
	PG_RETURN_TIMETZADT_P(time2);
}

Datum
timetz_smaller(PG_FUNCTION_ARGS)
{
	TimeTzADT   *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT   *time2 = PG_GETARG_TIMETZADT_P(1);

	if (DatumGetBool(DirectFunctionCall2(timetz_lt,
										 TimeTzADTPGetDatum(time1),
										 TimeTzADTPGetDatum(time2))))
		PG_RETURN_TIMETZADT_P(time1);
	PG_RETURN_TIMETZADT_P(time2);
}

/* overlaps_timetz()
 * Implements the SQL92 OVERLAPS operator.
 * Algorithm from Date and Darwen, 1997
 */
Datum
overlaps_timetz(PG_FUNCTION_ARGS)
{
	/* The arguments are TimeTzADT *, but we leave them as generic Datums
	 * for convenience of notation.
	 */
	Datum		ts1 = PG_GETARG_DATUM(0);
	Datum		te1 = PG_GETARG_DATUM(1);
	Datum		ts2 = PG_GETARG_DATUM(2);
	Datum		te2 = PG_GETARG_DATUM(3);

#define TIMETZ_GT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timetz_gt,t1,t2))
#define TIMETZ_LT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timetz_lt,t1,t2))
#define TIMETZ_EQ(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timetz_eq,t1,t2))

	/* Make sure we have ordered pairs... */
	if (TIMETZ_GT(ts1, te1))
	{
		Datum		tt = ts1;

		ts1 = te1;
		te1 = tt;
	}
	if (TIMETZ_GT(ts2, te2))
	{
		Datum		tt = ts2;

		ts2 = te2;
		te2 = tt;
	}

	PG_RETURN_BOOL((TIMETZ_GT(ts1, ts2) &&
					(TIMETZ_LT(ts1, te2) || TIMETZ_LT(te1, te2))) ||
				   (TIMETZ_GT(ts2, ts1) &&
					(TIMETZ_LT(ts2, te1) || TIMETZ_LT(te2, te1))) ||
				   TIMETZ_EQ(ts1, ts2));

#undef TIMETZ_GT
#undef TIMETZ_LT
#undef TIMETZ_EQ
}

/* timestamp_timetz()
 * Convert timestamp to timetz data type.
 */
Datum
timestamp_timetz(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	TimeTzADT  *result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	double		fsec;
	char	   *tzn;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		elog(ERROR, "Unable to convert timestamp to date");

	if (TIMESTAMP_IS_EPOCH(timestamp))
	{
		timestamp2tm(SetTimestamp(timestamp), NULL, tm, &fsec, NULL);
		tz = 0;
	}
	else if (TIMESTAMP_IS_CURRENT(timestamp))
		timestamp2tm(SetTimestamp(timestamp), &tz, tm, &fsec, &tzn);
	else
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, &tzn) != 0)
			elog(ERROR, "Unable to convert timestamp to date");
	}

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	result->time = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec + fsec);
	result->zone = tz;

	PG_RETURN_TIMETZADT_P(result);
}


/* datetimetz_timestamp()
 * Convert date and timetz to timestamp data type.
 * Timestamp is stored in GMT, so add the time zone
 * stored with the timetz to the result.
 * - thomas 2000-03-10
 */
Datum
datetimetz_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(1);
	Timestamp	result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	double		fsec = 0;
	char	   *tzn;

	if (date2tm(date, &tz, tm, &fsec, &tzn) != 0)
		elog(ERROR, "Unable to convert date to timestamp");

	if (tm2timestamp(tm, fsec, &time->zone, &result) != 0)
		elog(ERROR, "Timestamp out of range");

	result += time->time;

	PG_RETURN_TIMESTAMP(result);
}
