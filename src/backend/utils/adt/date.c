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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/date.c,v 1.43 2000/03/14 23:06:35 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <limits.h>

#include "postgres.h"
#ifdef HAVE_FLOAT_H
#include <float.h>
#endif
#include "miscadmin.h"
#include "utils/builtins.h"

static int
date2tm(DateADT dateVal, int *tzp, struct tm * tm, double *fsec, char **tzn);


/*****************************************************************************
 *	 Date ADT
 *****************************************************************************/


/* date_in()
 * Given date text string, convert to internal date format.
 */
DateADT
date_in(char *str)
{
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

	if (!PointerIsValid(str))
		elog(ERROR, "Bad (null) date external representation");

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

	return date;
}	/* date_in() */

/* date_out()
 * Given internal format date, convert to text string.
 */
char *
date_out(DateADT date)
{
	char	   *result;
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];

	j2date((date + date2j(2000, 1, 1)),
		   &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));

	EncodeDateOnly(tm, DateStyle, buf);

	result = palloc(strlen(buf) + 1);

	strcpy(result, buf);

	return result;
}	/* date_out() */

bool
date_eq(DateADT dateVal1, DateADT dateVal2)
{
	return dateVal1 == dateVal2;
}

bool
date_ne(DateADT dateVal1, DateADT dateVal2)
{
	return dateVal1 != dateVal2;
}

bool
date_lt(DateADT dateVal1, DateADT dateVal2)
{
	return dateVal1 < dateVal2;
}	/* date_lt() */

bool
date_le(DateADT dateVal1, DateADT dateVal2)
{
	return dateVal1 <= dateVal2;
}	/* date_le() */

bool
date_gt(DateADT dateVal1, DateADT dateVal2)
{
	return dateVal1 > dateVal2;
}	/* date_gt() */

bool
date_ge(DateADT dateVal1, DateADT dateVal2)
{
	return dateVal1 >= dateVal2;
}	/* date_ge() */

int
date_cmp(DateADT dateVal1, DateADT dateVal2)
{
	if (dateVal1 < dateVal2)
		return -1;
	else if (dateVal1 > dateVal2)
		return 1;
	return 0;
}	/* date_cmp() */

DateADT
date_larger(DateADT dateVal1, DateADT dateVal2)
{
	return date_gt(dateVal1, dateVal2) ? dateVal1 : dateVal2;
}	/* date_larger() */

DateADT
date_smaller(DateADT dateVal1, DateADT dateVal2)
{
	return date_lt(dateVal1, dateVal2) ? dateVal1 : dateVal2;
}	/* date_smaller() */

/* Compute difference between two dates in days.
 */
int4
date_mi(DateADT dateVal1, DateADT dateVal2)
{
	return dateVal1 - dateVal2;
}	/* date_mi() */

/* Add a number of days to a date, giving a new date.
 * Must handle both positive and negative numbers of days.
 */
DateADT
date_pli(DateADT dateVal, int4 days)
{
	return dateVal + days;
}	/* date_pli() */

/* Subtract a number of days from a date, giving a new date.
 */
DateADT
date_mii(DateADT dateVal, int4 days)
{
	return date_pli(dateVal, -days);
}	/* date_mii() */


/* date_timestamp()
 * Convert date to timestamp data type.
 */
Timestamp   *
date_timestamp(DateADT dateVal)
{
	Timestamp   *result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	double		fsec = 0;
	char	   *tzn;

	result = palloc(sizeof(*result));

	if (date2tm(dateVal, &tz, tm, &fsec, &tzn) != 0)
		elog(ERROR, "Unable to convert date to timestamp");

	if (tm2timestamp(tm, fsec, &tz, result) != 0)
		elog(ERROR, "Timestamp out of range");

	return result;
}	/* date_timestamp() */


/* timestamp_date()
 * Convert timestamp to date data type.
 */
DateADT
timestamp_date(Timestamp *timestamp)
{
	DateADT		result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	double		fsec;
	char	   *tzn;

	if (!PointerIsValid(timestamp))
		elog(ERROR, "Unable to convert null timestamp to date");

	if (TIMESTAMP_NOT_FINITE(*timestamp))
		elog(ERROR, "Unable to convert timestamp to date");

	if (TIMESTAMP_IS_EPOCH(*timestamp))
	{
		timestamp2tm(SetTimestamp(*timestamp), NULL, tm, &fsec, NULL);

	}
	else if (TIMESTAMP_IS_CURRENT(*timestamp))
	{
		timestamp2tm(SetTimestamp(*timestamp), &tz, tm, &fsec, &tzn);

	}
	else
	{
		if (timestamp2tm(*timestamp, &tz, tm, &fsec, &tzn) != 0)
			elog(ERROR, "Unable to convert timestamp to date");
	}

	result = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000, 1, 1));

	return result;
}	/* timestamp_date() */


/* abstime_date()
 * Convert abstime to date data type.
 */
DateADT
abstime_date(AbsoluteTime abstime)
{
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

	return result;
}	/* abstime_date() */


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


TimeADT    *
time_in(char *str)
{
	TimeADT    *time;

	double		fsec;
	struct tm	tt,
			   *tm = &tt;

	int			nf;
	char		lowstr[MAXDATELEN + 1];
	char	   *field[MAXDATEFIELDS];
	int			dtype;
	int			ftype[MAXDATEFIELDS];

	if (!PointerIsValid(str))
		elog(ERROR, "Bad (null) time external representation");

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
		|| (DecodeTimeOnly(field, ftype, nf, &dtype, tm, &fsec, NULL) != 0))
		elog(ERROR, "Bad time external representation '%s'", str);

	time = palloc(sizeof(*time));

	*time = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec + fsec);

	return time;
}	/* time_in() */


char *
time_out(TimeADT *time)
{
	char	   *result;
	struct tm	tt,
			   *tm = &tt;

	double		fsec;
	char		buf[MAXDATELEN + 1];

	if (!PointerIsValid(time))
		return NULL;

	tm->tm_hour = (*time / (60 * 60));
	tm->tm_min = (((int) (*time / 60)) % 60);
	tm->tm_sec = (((int) *time) % 60);

	fsec = 0;

	EncodeTimeOnly(tm, fsec, NULL, DateStyle, buf);

	result = palloc(strlen(buf) + 1);

	strcpy(result, buf);

	return result;
}	/* time_out() */


bool
time_eq(TimeADT *time1, TimeADT *time2)
{
	if (!PointerIsValid(time1) || !PointerIsValid(time2))
		return FALSE;

	return *time1 == *time2;
}	/* time_eq() */

bool
time_ne(TimeADT *time1, TimeADT *time2)
{
	if (!PointerIsValid(time1) || !PointerIsValid(time2))
		return FALSE;

	return *time1 != *time2;
}	/* time_eq() */

bool
time_lt(TimeADT *time1, TimeADT *time2)
{
	if (!PointerIsValid(time1) || !PointerIsValid(time2))
		return FALSE;

	return (*time1 < *time2);
}	/* time_lt() */

bool
time_le(TimeADT *time1, TimeADT *time2)
{
	if (!PointerIsValid(time1) || !PointerIsValid(time2))
		return FALSE;

	return (*time1 <= *time2);
}	/* time_le() */

bool
time_gt(TimeADT *time1, TimeADT *time2)
{
	if (!PointerIsValid(time1) || !PointerIsValid(time2))
		return FALSE;

	return (*time1 > *time2);
}	/* time_gt() */

bool
time_ge(TimeADT *time1, TimeADT *time2)
{
	if (!PointerIsValid(time1) || !PointerIsValid(time2))
		return FALSE;

	return (*time1 >= *time2);
}	/* time_ge() */

int
time_cmp(TimeADT *time1, TimeADT *time2)
{
	return (*time1 < *time2) ? -1 : (((*time1 > *time2) ? 1 : 0));
}	/* time_cmp() */

TimeADT *
time_larger(TimeADT *time1, TimeADT *time2)
{
	return time_gt(time1, time2)? time1: time2;
}	/* time_larger() */

TimeADT *
time_smaller(TimeADT *time1, TimeADT *time2)
{
	return time_lt(time1, time2)? time1: time2;
}	/* time_smaller() */

/* overlaps_time()
 * Implements the SQL92 OVERLAPS operator.
 * Algorithm from Date and Darwen, 1997
 */
bool
overlaps_time(TimeADT *ts1, TimeADT *te1, TimeADT *ts2, TimeADT *te2)
{
	/* Make sure we have ordered pairs... */
	if (time_gt(ts1, te1))
	{
		TimeADT *tt = ts1;
		ts1 = te1;
		te1 = tt;
	}
	if (time_gt(ts2, te2))
	{
		TimeADT *tt = ts2;
		ts2 = te2;
		te2 = tt;
	}

	return ((time_gt(ts1, ts2) && (time_lt(ts1, te2) || time_lt(te1, te2)))
			|| (time_gt(ts2, ts1) && (time_lt(ts2, te1) || time_lt(te2, te1)))
			|| time_eq(ts1, ts2));
}

/* timestamp_time()
 * Convert timestamp to time data type.
 */
TimeADT    *
timestamp_time(Timestamp *timestamp)
{
	TimeADT    *result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	double		fsec;
	char	   *tzn;

	if (!PointerIsValid(timestamp))
		elog(ERROR, "Unable to convert null timestamp to date");

	if (TIMESTAMP_NOT_FINITE(*timestamp))
		elog(ERROR, "Unable to convert timestamp to date");

	if (TIMESTAMP_IS_EPOCH(*timestamp))
	{
		timestamp2tm(SetTimestamp(*timestamp), NULL, tm, &fsec, NULL);
	}
	else if (TIMESTAMP_IS_CURRENT(*timestamp))
	{
		timestamp2tm(SetTimestamp(*timestamp), &tz, tm, &fsec, &tzn);
	}
	else
	{
		if (timestamp2tm(*timestamp, &tz, tm, &fsec, &tzn) != 0)
			elog(ERROR, "Unable to convert timestamp to date");
	}

	result = palloc(sizeof(*result));

	*result = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec + fsec);

	return result;
}	/* timestamp_time() */


/* datetime_timestamp()
 * Convert date and time to timestamp data type.
 */
Timestamp   *
datetime_timestamp(DateADT date, TimeADT *time)
{
	Timestamp   *result;

	if (!PointerIsValid(time))
	{
		result = palloc(sizeof(*result));
		TIMESTAMP_INVALID(*result);
	}
	else
	{
		result = date_timestamp(date);
		*result += *time;
	}

	return result;
}	/* datetime_timestamp() */


/* time_interval()
 * Convert time to interval data type.
 */
Interval   *
time_interval(TimeADT *time)
{
	Interval   *result;

	if (!PointerIsValid(time))
		return NULL;

	result = palloc(sizeof(*result));
	result->time = *time;
	result->month = 0;

	return result;
}	/* time_interval() */


/*****************************************************************************
 *	 Time With Time Zone ADT
 *****************************************************************************/


TimeTzADT    *
timetz_in(char *str)
{
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

	if (!PointerIsValid(str))
		elog(ERROR, "Bad (null) time external representation");

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
		|| (DecodeTimeOnly(field, ftype, nf, &dtype, tm, &fsec, &tz) != 0))
		elog(ERROR, "Bad time external representation '%s'", str);

	time = palloc(sizeof(*time));

	time->time = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec + fsec);
	time->zone = tz;

	return time;
}	/* timetz_in() */


char *
timetz_out(TimeTzADT *time)
{
	char	   *result;
	struct tm	tt,
			   *tm = &tt;

	double		fsec;
	int			tz;
	char		buf[MAXDATELEN + 1];

	if (!PointerIsValid(time))
		return NULL;

	tm->tm_hour = (time->time / (60 * 60));
	tm->tm_min = (((int) (time->time / 60)) % 60);
	tm->tm_sec = (((int) time->time) % 60);

	fsec = 0;
	tz = time->zone;

	EncodeTimeOnly(tm, fsec, &tz, DateStyle, buf);

	result = palloc(strlen(buf) + 1);

	strcpy(result, buf);

	return result;
}	/* timetz_out() */


bool
timetz_eq(TimeTzADT *time1, TimeTzADT *time2)
{
	if (!PointerIsValid(time1) || !PointerIsValid(time2))
		return FALSE;

	return ((time1->time + time1->zone) == (time2->time + time2->zone));
}	/* timetz_eq() */

bool
timetz_ne(TimeTzADT *time1, TimeTzADT *time2)
{
	if (!PointerIsValid(time1) || !PointerIsValid(time2))
		return FALSE;

	return ((time1->time + time1->zone) != (time2->time + time2->zone));
}	/* timetz_ne() */

bool
timetz_lt(TimeTzADT *time1, TimeTzADT *time2)
{
	if (!PointerIsValid(time1) || !PointerIsValid(time2))
		return FALSE;

	return ((time1->time + time1->zone) < (time2->time + time2->zone));
}	/* timetz_lt() */

bool
timetz_le(TimeTzADT *time1, TimeTzADT *time2)
{
	if (!PointerIsValid(time1) || !PointerIsValid(time2))
		return FALSE;

	return ((time1->time + time1->zone) <= (time2->time + time2->zone));
}	/* timetz_le() */

bool
timetz_gt(TimeTzADT *time1, TimeTzADT *time2)
{
	if (!PointerIsValid(time1) || !PointerIsValid(time2))
		return FALSE;

	return ((time1->time + time1->zone) > (time2->time + time2->zone));
}	/* timetz_gt() */

bool
timetz_ge(TimeTzADT *time1, TimeTzADT *time2)
{
	if (!PointerIsValid(time1) || !PointerIsValid(time2))
		return FALSE;

	return ((time1->time + time1->zone) >= (time2->time + time2->zone));
}	/* timetz_ge() */

int
timetz_cmp(TimeTzADT *time1, TimeTzADT *time2)
{
	return (timetz_lt(time1, time2) ? -1 : (timetz_gt(time1, time2)? 1: 0));
}	/* timetz_cmp() */

TimeTzADT *
timetz_larger(TimeTzADT *time1, TimeTzADT *time2)
{
	return timetz_gt(time1, time2)? time1: time2;
}	/* timetz_larger() */

TimeTzADT *
timetz_smaller(TimeTzADT *time1, TimeTzADT *time2)
{
	return timetz_lt(time1, time2)? time1: time2;
}	/* timetz_smaller() */

/* overlaps_timetz()
 * Implements the SQL92 OVERLAPS operator.
 * Algorithm from Date and Darwen, 1997
 */
bool
overlaps_timetz(TimeTzADT *ts1, TimeTzADT *te1, TimeTzADT *ts2, TimeTzADT *te2)
{
	/* Make sure we have ordered pairs... */
	if (timetz_gt(ts1, te1))
	{
		TimeTzADT *tt = ts1;
		ts1 = te1;
		te1 = tt;
	}
	if (timetz_gt(ts2, te2))
	{
		TimeTzADT *tt = ts2;
		ts2 = te2;
		te2 = tt;
	}

	return ((timetz_gt(ts1, ts2) && (timetz_lt(ts1, te2) || timetz_lt(te1, te2)))
			|| (timetz_gt(ts2, ts1) && (timetz_lt(ts2, te1) || timetz_lt(te2, te1)))
			|| timetz_eq(ts1, ts2));
}	/* overlaps_timetz() */

/* timestamp_timetz()
 * Convert timestamp to timetz data type.
 */
TimeTzADT    *
timestamp_timetz(Timestamp *timestamp)
{
	TimeTzADT    *result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	double		fsec;
	char	   *tzn;

	if (!PointerIsValid(timestamp))
		elog(ERROR, "Unable to convert null timestamp to date");

	if (TIMESTAMP_NOT_FINITE(*timestamp))
		elog(ERROR, "Unable to convert timestamp to date");

	if (TIMESTAMP_IS_EPOCH(*timestamp))
	{
		timestamp2tm(SetTimestamp(*timestamp), NULL, tm, &fsec, NULL);
		tz = 0;
	}
	else if (TIMESTAMP_IS_CURRENT(*timestamp))
	{
		timestamp2tm(SetTimestamp(*timestamp), &tz, tm, &fsec, &tzn);
	}
	else
	{
		if (timestamp2tm(*timestamp, &tz, tm, &fsec, &tzn) != 0)
			elog(ERROR, "Unable to convert timestamp to date");
	}

	result = palloc(sizeof(*result));

	result->time = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec + fsec);
	result->zone = tz;

	return result;
}	/* timestamp_timetz() */


/* datetimetz_timestamp()
 * Convert date and timetz to timestamp data type.
 * Timestamp is stored in GMT, so add the time zone
 * stored with the timetz to the result.
 * - thomas 2000-03-10
 */
Timestamp   *
datetimetz_timestamp(DateADT date, TimeTzADT *time)
{
	Timestamp  *result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	double		fsec = 0;
	char	   *tzn;

	result = palloc(sizeof(*result));

	if (!PointerIsValid(date) || !PointerIsValid(time))
	{
		TIMESTAMP_INVALID(*result);
	}
	else
	{
		if (date2tm(date, &tz, tm, &fsec, &tzn) != 0)
			elog(ERROR, "Unable to convert date to timestamp");

		if (tm2timestamp(tm, fsec, &time->zone, result) != 0)
			elog(ERROR, "Timestamp out of range");

		*result += time->time;
	}

	return result;
}	/* datetimetz_timestamp() */


