/*-------------------------------------------------------------------------
 *
 * timestamp.c
 *	  Functions for the built-in SQL92 type "timestamp" and "interval".
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/timestamp.c,v 1.26 2000/04/14 15:22:10 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <math.h>
#include <sys/types.h>
#include <errno.h>

#include "postgres.h"
#ifdef HAVE_FLOAT_H
#include <float.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#ifndef USE_POSIX_TIME
#include <sys/timeb.h>
#endif

#include "miscadmin.h"
#include "utils/builtins.h"


static double time2t(const int hour, const int min, const double sec);


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/* timestamp_in()
 * Convert a string to internal form.
 */
Timestamp  *
timestamp_in(char *str)
{
	Timestamp  *result;

	double		fsec;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	int			dtype;
	int			nf;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + 1];

	if (!PointerIsValid(str))
		elog(ERROR, "Bad (null) timestamp external representation");

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
	  || (DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz) != 0))
		elog(ERROR, "Bad timestamp external representation '%s'", str);

	result = palloc(sizeof(Timestamp));

	switch (dtype)
	{
		case DTK_DATE:
			if (tm2timestamp(tm, fsec, &tz, result) != 0)
				elog(ERROR, "Timestamp out of range '%s'", str);
			break;

		case DTK_EPOCH:
			TIMESTAMP_EPOCH(*result);
			break;

		case DTK_CURRENT:
			TIMESTAMP_CURRENT(*result);
			break;

		case DTK_LATE:
			TIMESTAMP_NOEND(*result);
			break;

		case DTK_EARLY:
			TIMESTAMP_NOBEGIN(*result);
			break;

		case DTK_INVALID:
			TIMESTAMP_INVALID(*result);
			break;

		default:
			elog(ERROR, "Internal coding error, can't input timestamp '%s'", str);
	}

	return result;
}	/* timestamp_in() */

/* timestamp_out()
 * Convert a timestamp to external form.
 */
char *
timestamp_out(Timestamp *dt)
{
	char	   *result;
	int			tz;
	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	char	   *tzn;
	char		buf[MAXDATELEN + 1];

	if (!PointerIsValid(dt))
		return NULL;

	if (TIMESTAMP_IS_RESERVED(*dt))
	{
		EncodeSpecialTimestamp(*dt, buf);

	}
	else if (timestamp2tm(*dt, &tz, tm, &fsec, &tzn) == 0)
	{
		EncodeDateTime(tm, fsec, &tz, &tzn, DateStyle, buf);

	}
	else
		EncodeSpecialTimestamp(DT_INVALID, buf);

	result = palloc(strlen(buf) + 1);

	strcpy(result, buf);

	return result;
}	/* timestamp_out() */


/* interval_in()
 * Convert a string to internal form.
 *
 * External format(s):
 *	Uses the generic date/time parsing and decoding routines.
 */
Interval   *
interval_in(char *str)
{
	Interval   *span;

	double		fsec;
	struct tm	tt,
			   *tm = &tt;
	int			dtype;
	int			nf;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + 1];

	tm->tm_year = 0;
	tm->tm_mon = 0;
	tm->tm_mday = 0;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	fsec = 0;

	if (!PointerIsValid(str))
		elog(ERROR, "Bad (null) interval external representation");

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
		|| (DecodeDateDelta(field, ftype, nf, &dtype, tm, &fsec) != 0))
		elog(ERROR, "Bad interval external representation '%s'", str);

	span = palloc(sizeof(Interval));

	switch (dtype)
	{
		case DTK_DELTA:
			if (tm2interval(tm, fsec, span) != 0)
			{
#if NOT_USED
				INTERVAL_INVALID(span);
#endif
				elog(ERROR, "Bad interval external representation '%s'", str);
			}
			break;

		default:
			elog(ERROR, "Internal coding error, can't input interval '%s'", str);
	}

	return span;
}	/* interval_in() */

/* interval_out()
 * Convert a time span to external form.
 */
char *
interval_out(Interval *span)
{
	char	   *result;

	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	char		buf[MAXDATELEN + 1];

	if (!PointerIsValid(span))
		return NULL;

	if (interval2tm(*span, tm, &fsec) != 0)
		return NULL;

	if (EncodeTimeSpan(tm, fsec, DateStyle, buf) != 0)
		elog(ERROR, "Unable to format interval");

	result = palloc(strlen(buf) + 1);

	strcpy(result, buf);
	return result;
}	/* interval_out() */


/* EncodeSpecialTimestamp()
 * Convert reserved timestamp data type to string.
 */
int
EncodeSpecialTimestamp(Timestamp dt, char *str)
{
	if (TIMESTAMP_IS_RESERVED(dt))
	{
		if (TIMESTAMP_IS_INVALID(dt))
			strcpy(str, INVALID);
		else if (TIMESTAMP_IS_NOBEGIN(dt))
			strcpy(str, EARLY);
		else if (TIMESTAMP_IS_NOEND(dt))
			strcpy(str, LATE);
		else if (TIMESTAMP_IS_CURRENT(dt))
			strcpy(str, DCURRENT);
		else if (TIMESTAMP_IS_EPOCH(dt))
			strcpy(str, EPOCH);
		else
			strcpy(str, INVALID);
		return TRUE;
	}

	return FALSE;
}	/* EncodeSpecialTimestamp() */

Timestamp  *
now(void)
{
	Timestamp  *result;
	AbsoluteTime sec;

	result = palloc(sizeof(Timestamp));

	sec = GetCurrentTransactionStartTime();

	*result = (sec - ((date2j(2000, 1, 1) - date2j(1970, 1, 1)) * 86400));

	return result;
}

void
dt2time(Timestamp jd, int *hour, int *min, double *sec)
{
	double		time;

	time = jd;

	*hour = (time / 3600);
	time -= ((*hour) * 3600);
	*min = (time / 60);
	time -= ((*min) * 60);
	*sec = JROUND(time);

	return;
}	/* dt2time() */


/* timestamp2tm()
 * Convert timestamp data type to POSIX time structure.
 * Note that year is _not_ 1900-based, but is an explicit full value.
 * Also, month is one-based, _not_ zero-based.
 * Returns:
 *	 0 on success
 *	-1 on out of range
 *
 * For dates within the system-supported time_t range, convert to the
 *	local time zone. If out of this range, leave as GMT. - tgl 97/05/27
 */
int
timestamp2tm(Timestamp dt, int *tzp, struct tm * tm, double *fsec, char **tzn)
{
	double		date,
				date0,
				time,
				sec;
	time_t		utime;

#ifdef USE_POSIX_TIME
	struct tm  *tx;

#endif

	date0 = date2j(2000, 1, 1);

	time = dt;
	TMODULO(time, date, 86400e0);

	if (time < 0)
	{
		time += 86400;
		date -= 1;
	}

	/* Julian day routine does not work for negative Julian days */
	if (date < -date0)
		return -1;

	/* add offset to go from J2000 back to standard Julian date */
	date += date0;

	j2date((int) date, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
	dt2time(time, &tm->tm_hour, &tm->tm_min, &sec);

	*fsec = JROUND(sec);
	TMODULO(*fsec, tm->tm_sec, 1e0);

	if (tzp != NULL)
	{
		if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
		{
			utime = (dt + (date0 - date2j(1970, 1, 1)) * 86400);

#ifdef USE_POSIX_TIME
			tx = localtime(&utime);
			tm->tm_year = tx->tm_year + 1900;
			tm->tm_mon = tx->tm_mon + 1;
			tm->tm_mday = tx->tm_mday;
			tm->tm_hour = tx->tm_hour;
			tm->tm_min = tx->tm_min;
#if NOT_USED
/* XXX HACK
 * Argh! My Linux box puts in a 1 second offset for dates less than 1970
 *	but only if the seconds field was non-zero. So, don't copy the seconds
 *	field and instead carry forward from the original - tgl 97/06/18
 * Note that GNU/Linux uses the standard freeware zic package as do
 *	many other platforms so this may not be GNU/Linux/ix86-specific.
 */
			tm->tm_sec = tx->tm_sec;
#endif
			tm->tm_isdst = tx->tm_isdst;

#if defined(HAVE_TM_ZONE)
			tm->tm_gmtoff = tx->tm_gmtoff;
			tm->tm_zone = tx->tm_zone;

			*tzp = -(tm->tm_gmtoff);	/* tm_gmtoff is Sun/DEC-ism */
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
			*tzp = CTimeZone;	/* V7 conventions; don't know timezone? */
			if (tzn != NULL)
				*tzn = CTZName;
#endif

		}
		else
		{
			*tzp = 0;
			tm->tm_isdst = 0;
			if (tzn != NULL)
				*tzn = NULL;
		}

		dt = dt2local(dt, *tzp);

	}
	else
	{
		tm->tm_isdst = 0;
		if (tzn != NULL)
			*tzn = NULL;
	}

	return 0;
}	/* timestamp2tm() */


/* tm2timestamp()
 * Convert a tm structure to a timestamp data type.
 * Note that year is _not_ 1900-based, but is an explicit full value.
 * Also, month is one-based, _not_ zero-based.
 */
int
tm2timestamp(struct tm * tm, double fsec, int *tzp, Timestamp *result)
{

	double		date,
				time;

	/* Julian day routines are not correct for negative Julian days */
	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
		return -1;

	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000, 1, 1);
	time = time2t(tm->tm_hour, tm->tm_min, (tm->tm_sec + fsec));
	*result = (date * 86400 + time);
	if (tzp != NULL)
		*result = dt2local(*result, -(*tzp));

	return 0;
}	/* tm2timestamp() */


/* interval2tm()
 * Convert a interval data type to a tm structure.
 */
int
interval2tm(Interval span, struct tm * tm, float8 *fsec)
{
	double		time;

	if (span.month != 0)
	{
		tm->tm_year = span.month / 12;
		tm->tm_mon = span.month % 12;

	}
	else
	{
		tm->tm_year = 0;
		tm->tm_mon = 0;
	}

#ifdef ROUND_ALL
	time = JROUND(span.time);
#else
	time = span.time;
#endif

	TMODULO(time, tm->tm_mday, 86400e0);
	TMODULO(time, tm->tm_hour, 3600e0);
	TMODULO(time, tm->tm_min, 60e0);
	TMODULO(time, tm->tm_sec, 1e0);
	*fsec = time;

	return 0;
}	/* interval2tm() */

int
tm2interval(struct tm * tm, double fsec, Interval *span)
{
	span->month = ((tm->tm_year * 12) + tm->tm_mon);
	span->time = ((((((tm->tm_mday * 24.0)
					  + tm->tm_hour) * 60.0)
					+ tm->tm_min) * 60.0)
				  + tm->tm_sec);
	span->time = JROUND(span->time + fsec);

	return 0;
}	/* tm2interval() */

static double
time2t(const int hour, const int min, const double sec)
{
	return (((hour * 60) + min) * 60) + sec;
}	/* time2t() */

Timestamp
dt2local(Timestamp dt, int tz)
{
	dt -= tz;
	dt = JROUND(dt);
	return dt;
}	/* dt2local() */


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/


bool
timestamp_finite(Timestamp *timestamp)
{
	if (!PointerIsValid(timestamp))
		return FALSE;

	return !TIMESTAMP_NOT_FINITE(*timestamp);
}	/* timestamp_finite() */

bool
interval_finite(Interval *interval)
{
	if (!PointerIsValid(interval))
		return FALSE;

	return !INTERVAL_NOT_FINITE(*interval);
}	/* interval_finite() */


/*----------------------------------------------------------
 *	Relational operators for timestamp.
 *---------------------------------------------------------*/

static void
GetEpochTime(struct tm * tm)
{
	struct tm  *t0;
	time_t		epoch = 0;

	t0 = gmtime(&epoch);

	tm->tm_year = t0->tm_year;
	tm->tm_mon = t0->tm_mon;
	tm->tm_mday = t0->tm_mday;
	tm->tm_hour = t0->tm_hour;
	tm->tm_min = t0->tm_min;
	tm->tm_sec = t0->tm_sec;

	if (tm->tm_year < 1900)
		tm->tm_year += 1900;
	tm->tm_mon++;

	return;
}	/* GetEpochTime() */

Timestamp
SetTimestamp(Timestamp dt)
{
	struct tm	tt;

	if (TIMESTAMP_IS_CURRENT(dt))
	{
		GetCurrentTime(&tt);
		tm2timestamp(&tt, 0, NULL, &dt);
		dt = dt2local(dt, -CTimeZone);
	}
	else
	{							/* if (TIMESTAMP_IS_EPOCH(dt1)) */
		GetEpochTime(&tt);
		tm2timestamp(&tt, 0, NULL, &dt);
	}

	return dt;
}	/* SetTimestamp() */

/*		timestamp_relop - is timestamp1 relop timestamp2
 */
bool
timestamp_eq(Timestamp *timestamp1, Timestamp *timestamp2)
{
	Timestamp	dt1,
				dt2;

	if (!PointerIsValid(timestamp1) || !PointerIsValid(timestamp2))
		return FALSE;

	dt1 = *timestamp1;
	dt2 = *timestamp2;

	if (TIMESTAMP_IS_INVALID(dt1) || TIMESTAMP_IS_INVALID(dt2))
		return FALSE;

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	return dt1 == dt2;
}	/* timestamp_eq() */

bool
timestamp_ne(Timestamp *timestamp1, Timestamp *timestamp2)
{
	Timestamp	dt1,
				dt2;

	if (!PointerIsValid(timestamp1) || !PointerIsValid(timestamp2))
		return FALSE;

	dt1 = *timestamp1;
	dt2 = *timestamp2;

	if (TIMESTAMP_IS_INVALID(dt1) || TIMESTAMP_IS_INVALID(dt2))
		return FALSE;

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	return dt1 != dt2;
}	/* timestamp_ne() */

bool
timestamp_lt(Timestamp *timestamp1, Timestamp *timestamp2)
{
	Timestamp	dt1,
				dt2;

	if (!PointerIsValid(timestamp1) || !PointerIsValid(timestamp2))
		return FALSE;

	dt1 = *timestamp1;
	dt2 = *timestamp2;

	if (TIMESTAMP_IS_INVALID(dt1) || TIMESTAMP_IS_INVALID(dt2))
		return FALSE;

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	return dt1 < dt2;
}	/* timestamp_lt() */

bool
timestamp_gt(Timestamp *timestamp1, Timestamp *timestamp2)
{
	Timestamp	dt1,
				dt2;

	if (!PointerIsValid(timestamp1) || !PointerIsValid(timestamp2))
		return FALSE;

	dt1 = *timestamp1;
	dt2 = *timestamp2;

	if (TIMESTAMP_IS_INVALID(dt1) || TIMESTAMP_IS_INVALID(dt2))
		return FALSE;

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	return dt1 > dt2;
}	/* timestamp_gt() */

bool
timestamp_le(Timestamp *timestamp1, Timestamp *timestamp2)
{
	Timestamp	dt1,
				dt2;

	if (!PointerIsValid(timestamp1) || !PointerIsValid(timestamp2))
		return FALSE;

	dt1 = *timestamp1;
	dt2 = *timestamp2;

	if (TIMESTAMP_IS_INVALID(dt1) || TIMESTAMP_IS_INVALID(dt2))
		return FALSE;

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	return dt1 <= dt2;
}	/* timestamp_le() */

bool
timestamp_ge(Timestamp *timestamp1, Timestamp *timestamp2)
{
	Timestamp	dt1,
				dt2;

	if (!PointerIsValid(timestamp1) || !PointerIsValid(timestamp2))
		return FALSE;

	dt1 = *timestamp1;
	dt2 = *timestamp2;

	if (TIMESTAMP_IS_INVALID(dt1) || TIMESTAMP_IS_INVALID(dt2))
		return FALSE;

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	return dt1 >= dt2;
}	/* timestamp_ge() */


/*		timestamp_cmp	- 3-state comparison for timestamp
 *		collate invalid timestamp at the end
 */
int
timestamp_cmp(Timestamp *timestamp1, Timestamp *timestamp2)
{
	Timestamp	dt1,
				dt2;

	if (!PointerIsValid(timestamp1) || !PointerIsValid(timestamp2))
		return 0;

	dt1 = *timestamp1;
	dt2 = *timestamp2;

	if (TIMESTAMP_IS_INVALID(dt1))
	{
		return (TIMESTAMP_IS_INVALID(dt2) ? 0 : 1);

	}
	else if (TIMESTAMP_IS_INVALID(dt2))
	{
		return -1;

	}
	else
	{
		if (TIMESTAMP_IS_RELATIVE(dt1))
			dt1 = SetTimestamp(dt1);
		if (TIMESTAMP_IS_RELATIVE(dt2))
			dt2 = SetTimestamp(dt2);
	}

	return ((dt1 < dt2) ? -1 : ((dt1 > dt2) ? 1 : 0));
}	/* timestamp_cmp() */


/*		interval_relop	- is interval1 relop interval2
 */
bool
interval_eq(Interval *interval1, Interval *interval2)
{
	if (!PointerIsValid(interval1) || !PointerIsValid(interval2))
		return FALSE;

	if (INTERVAL_IS_INVALID(*interval1) || INTERVAL_IS_INVALID(*interval2))
		return FALSE;

	return ((interval1->time == interval2->time)
			&& (interval1->month == interval2->month));
}	/* interval_eq() */

bool
interval_ne(Interval *interval1, Interval *interval2)
{
	if (!PointerIsValid(interval1) || !PointerIsValid(interval2))
		return FALSE;

	if (INTERVAL_IS_INVALID(*interval1) || INTERVAL_IS_INVALID(*interval2))
		return FALSE;

	return ((interval1->time != interval2->time)
			|| (interval1->month != interval2->month));
}	/* interval_ne() */

bool
interval_lt(Interval *interval1, Interval *interval2)
{
	double		span1,
				span2;

	if (!PointerIsValid(interval1) || !PointerIsValid(interval2))
		return FALSE;

	if (INTERVAL_IS_INVALID(*interval1) || INTERVAL_IS_INVALID(*interval2))
		return FALSE;

	span1 = interval1->time;
	if (interval1->month != 0)
		span1 += (interval1->month * (30.0 * 86400));
	span2 = interval2->time;
	if (interval2->month != 0)
		span2 += (interval2->month * (30.0 * 86400));

	return span1 < span2;
}	/* interval_lt() */

bool
interval_gt(Interval *interval1, Interval *interval2)
{
	double		span1,
				span2;

	if (!PointerIsValid(interval1) || !PointerIsValid(interval2))
		return FALSE;

	if (INTERVAL_IS_INVALID(*interval1) || INTERVAL_IS_INVALID(*interval2))
		return FALSE;

	span1 = interval1->time;
	if (interval1->month != 0)
		span1 += (interval1->month * (30.0 * 86400));
	span2 = interval2->time;
	if (interval2->month != 0)
		span2 += (interval2->month * (30.0 * 86400));

	return span1 > span2;
}	/* interval_gt() */

bool
interval_le(Interval *interval1, Interval *interval2)
{
	double		span1,
				span2;

	if (!PointerIsValid(interval1) || !PointerIsValid(interval2))
		return FALSE;

	if (INTERVAL_IS_INVALID(*interval1) || INTERVAL_IS_INVALID(*interval2))
		return FALSE;

	span1 = interval1->time;
	if (interval1->month != 0)
		span1 += (interval1->month * (30.0 * 86400));
	span2 = interval2->time;
	if (interval2->month != 0)
		span2 += (interval2->month * (30.0 * 86400));

	return span1 <= span2;
}	/* interval_le() */

bool
interval_ge(Interval *interval1, Interval *interval2)
{
	double		span1,
				span2;

	if (!PointerIsValid(interval1) || !PointerIsValid(interval2))
		return FALSE;

	if (INTERVAL_IS_INVALID(*interval1) || INTERVAL_IS_INVALID(*interval2))
		return FALSE;

	span1 = interval1->time;
	if (interval1->month != 0)
		span1 += (interval1->month * (30.0 * 86400));
	span2 = interval2->time;
	if (interval2->month != 0)
		span2 += (interval2->month * (30.0 * 86400));

	return span1 >= span2;
}	/* interval_ge() */


/*		interval_cmp	- 3-state comparison for interval
 */
int
interval_cmp(Interval *interval1, Interval *interval2)
{
	double		span1,
				span2;

	if (!PointerIsValid(interval1) || !PointerIsValid(interval2))
		return 0;

	if (INTERVAL_IS_INVALID(*interval1))
	{
		return INTERVAL_IS_INVALID(*interval2) ? 0 : 1;

	}
	else if (INTERVAL_IS_INVALID(*interval2))
		return -1;

	span1 = interval1->time;
	if (interval1->month != 0)
		span1 += (interval1->month * (30.0 * 86400));
	span2 = interval2->time;
	if (interval2->month != 0)
		span2 += (interval2->month * (30.0 * 86400));

	return (span1 < span2) ? -1 : (span1 > span2) ? 1 : 0;
}	/* interval_cmp() */

/* overlaps_timestamp()
 * Implements the SQL92 OVERLAPS operator.
 * Algorithm from Date and Darwen, 1997
 */
bool
overlaps_timestamp(Timestamp *ts1, Timestamp *te1, Timestamp *ts2, Timestamp *te2)
{
	/* Make sure we have ordered pairs... */
	if (timestamp_gt(ts1, te1))
	{
		Timestamp  *tt = ts1;

		ts1 = te1;
		te1 = tt;
	}
	if (timestamp_gt(ts2, te2))
	{
		Timestamp  *tt = ts2;

		ts2 = te2;
		te2 = tt;
	}

	return ((timestamp_gt(ts1, ts2) && (timestamp_lt(ts1, te2) || timestamp_lt(te1, te2)))
			|| (timestamp_gt(ts2, ts1) && (timestamp_lt(ts2, te1) || timestamp_lt(te2, te1)))
			|| timestamp_eq(ts1, ts2));
}	/* overlaps_timestamp() */


/*----------------------------------------------------------
 *	"Arithmetic" operators on date/times.
 *		timestamp_foo	returns foo as an object (pointer) that
 *						can be passed between languages.
 *		timestamp_xx		is an internal routine which returns the
 *						actual value.
 *---------------------------------------------------------*/

Timestamp  *
timestamp_smaller(Timestamp *timestamp1, Timestamp *timestamp2)
{
	Timestamp  *result;

	Timestamp	dt1,
				dt2;

	if (!PointerIsValid(timestamp1) || !PointerIsValid(timestamp2))
		return NULL;

	dt1 = *timestamp1;
	dt2 = *timestamp2;

	result = palloc(sizeof(Timestamp));

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	if (TIMESTAMP_IS_INVALID(dt1))
		*result = dt2;
	else if (TIMESTAMP_IS_INVALID(dt2))
		*result = dt1;
	else
		*result = ((dt2 < dt1) ? dt2 : dt1);

	return result;
}	/* timestamp_smaller() */

Timestamp  *
timestamp_larger(Timestamp *timestamp1, Timestamp *timestamp2)
{
	Timestamp  *result;

	Timestamp	dt1,
				dt2;

	if (!PointerIsValid(timestamp1) || !PointerIsValid(timestamp2))
		return NULL;

	dt1 = *timestamp1;
	dt2 = *timestamp2;

	result = palloc(sizeof(Timestamp));

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	if (TIMESTAMP_IS_INVALID(dt1))
		*result = dt2;
	else if (TIMESTAMP_IS_INVALID(dt2))
		*result = dt1;
	else
		*result = ((dt2 > dt1) ? dt2 : dt1);

	return result;
}	/* timestamp_larger() */


Interval   *
timestamp_mi(Timestamp *timestamp1, Timestamp *timestamp2)
{
	Interval   *result;

	Timestamp	dt1,
				dt2;

	if (!PointerIsValid(timestamp1) || !PointerIsValid(timestamp2))
		return NULL;

	dt1 = *timestamp1;
	dt2 = *timestamp2;

	result = palloc(sizeof(Interval));

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	if (TIMESTAMP_IS_INVALID(dt1)
		|| TIMESTAMP_IS_INVALID(dt2))
	{
		TIMESTAMP_INVALID(result->time);

	}
	else
		result->time = JROUND(dt1 - dt2);
	result->month = 0;

	return result;
}	/* timestamp_mi() */


/* timestamp_pl_span()
 * Add a interval to a timestamp data type.
 * Note that interval has provisions for qualitative year/month
 *	units, so try to do the right thing with them.
 * To add a month, increment the month, and use the same day of month.
 * Then, if the next month has fewer days, set the day of month
 *	to the last day of month.
 * Lastly, add in the "quantitative time".
 */
Timestamp  *
timestamp_pl_span(Timestamp *timestamp, Interval *span)
{
	Timestamp  *result;
	Timestamp	dt;
	int			tz;
	char	   *tzn;

	if ((!PointerIsValid(timestamp)) || (!PointerIsValid(span)))
		return NULL;

	result = palloc(sizeof(Timestamp));

	if (TIMESTAMP_NOT_FINITE(*timestamp))
	{
		*result = *timestamp;

	}
	else if (INTERVAL_IS_INVALID(*span))
	{
		TIMESTAMP_INVALID(*result);

	}
	else
	{
		dt = (TIMESTAMP_IS_RELATIVE(*timestamp) ? SetTimestamp(*timestamp) : *timestamp);

		if (span->month != 0)
		{
			struct tm	tt,
					   *tm = &tt;
			double		fsec;

			if (timestamp2tm(dt, &tz, tm, &fsec, &tzn) == 0)
			{
				tm->tm_mon += span->month;
				if (tm->tm_mon > 12)
				{
					tm->tm_year += ((tm->tm_mon - 1) / 12);
					tm->tm_mon = (((tm->tm_mon - 1) % 12) + 1);
				}
				else if (tm->tm_mon < 1)
				{
					tm->tm_year += ((tm->tm_mon / 12) - 1);
					tm->tm_mon = ((tm->tm_mon % 12) + 12);
				}

				/* adjust for end of month boundary problems... */
				if (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
					tm->tm_mday = (day_tab[isleap(tm->tm_year)][tm->tm_mon - 1]);

				if (tm2timestamp(tm, fsec, &tz, &dt) != 0)
					elog(ERROR, "Unable to add timestamp and interval");

			}
			else
				TIMESTAMP_INVALID(dt);
		}

#ifdef ROUND_ALL
		dt = JROUND(dt + span->time);
#else
		dt += span->time;
#endif

		*result = dt;
	}

	return result;
}	/* timestamp_pl_span() */

Timestamp  *
timestamp_mi_span(Timestamp *timestamp, Interval *span)
{
	Timestamp  *result;
	Interval	tspan;

	if (!PointerIsValid(timestamp) || !PointerIsValid(span))
		return NULL;

	tspan.month = -span->month;
	tspan.time = -span->time;

	result = timestamp_pl_span(timestamp, &tspan);

	return result;
}	/* timestamp_mi_span() */


Interval   *
interval_um(Interval *interval)
{
	Interval   *result;

	if (!PointerIsValid(interval))
		return NULL;

	result = palloc(sizeof(Interval));

	result->time = -(interval->time);
	result->month = -(interval->month);

	return result;
}	/* interval_um() */


Interval   *
interval_smaller(Interval *interval1, Interval *interval2)
{
	Interval   *result;

	double		span1,
				span2;

	if (!PointerIsValid(interval1) || !PointerIsValid(interval2))
		return NULL;

	result = palloc(sizeof(Interval));

	if (INTERVAL_IS_INVALID(*interval1))
	{
		result->time = interval2->time;
		result->month = interval2->month;

	}
	else if (INTERVAL_IS_INVALID(*interval2))
	{
		result->time = interval1->time;
		result->month = interval1->month;

	}
	else
	{
		span1 = interval1->time;
		if (interval1->month != 0)
			span1 += (interval1->month * (30.0 * 86400));
		span2 = interval2->time;
		if (interval2->month != 0)
			span2 += (interval2->month * (30.0 * 86400));

		if (span2 < span1)
		{
			result->time = interval2->time;
			result->month = interval2->month;

		}
		else
		{
			result->time = interval1->time;
			result->month = interval1->month;
		}
	}

	return result;
}	/* interval_smaller() */

Interval   *
interval_larger(Interval *interval1, Interval *interval2)
{
	Interval   *result;

	double		span1,
				span2;

	if (!PointerIsValid(interval1) || !PointerIsValid(interval2))
		return NULL;

	result = palloc(sizeof(Interval));

	if (INTERVAL_IS_INVALID(*interval1))
	{
		result->time = interval2->time;
		result->month = interval2->month;

	}
	else if (INTERVAL_IS_INVALID(*interval2))
	{
		result->time = interval1->time;
		result->month = interval1->month;

	}
	else
	{
		span1 = interval1->time;
		if (interval1->month != 0)
			span1 += (interval1->month * (30.0 * 86400));
		span2 = interval2->time;
		if (interval2->month != 0)
			span2 += (interval2->month * (30.0 * 86400));

		if (span2 > span1)
		{
			result->time = interval2->time;
			result->month = interval2->month;

		}
		else
		{
			result->time = interval1->time;
			result->month = interval1->month;
		}
	}

	return result;
}	/* interval_larger() */


Interval   *
interval_pl(Interval *span1, Interval *span2)
{
	Interval   *result;

	if ((!PointerIsValid(span1)) || (!PointerIsValid(span2)))
		return NULL;

	result = palloc(sizeof(Interval));

	result->month = (span1->month + span2->month);
	result->time = JROUND(span1->time + span2->time);

	return result;
}	/* interval_pl() */

Interval   *
interval_mi(Interval *span1, Interval *span2)
{
	Interval   *result;

	if ((!PointerIsValid(span1)) || (!PointerIsValid(span2)))
		return NULL;

	result = palloc(sizeof(Interval));

	result->month = (span1->month - span2->month);
	result->time = JROUND(span1->time - span2->time);

	return result;
}	/* interval_mi() */

Interval   *
interval_mul(Interval *span1, float8 *factor)
{
	Interval   *result;
	double		months;

	if ((!PointerIsValid(span1)) || (!PointerIsValid(factor)))
		return NULL;

	if (!PointerIsValid(result = palloc(sizeof(Interval))))
		elog(ERROR, "Memory allocation failed, can't multiply interval");

	months = (span1->month * *factor);
	result->month = rint(months);
	result->time = JROUND(span1->time * *factor);
	/* evaluate fractional months as 30 days */
	result->time += JROUND((months - result->month) * 30 * 86400);

	return result;
}	/* interval_mul() */

Interval   *
mul_d_interval(float8 *factor, Interval *span1)
{
	return interval_mul(span1, factor);
}	/* mul_d_interval() */

Interval   *
interval_div(Interval *span1, float8 *factor)
{
	Interval   *result;
	double		months;

	if ((!PointerIsValid(span1)) || (!PointerIsValid(factor)))
		return NULL;

	if (!PointerIsValid(result = palloc(sizeof(Interval))))
		elog(ERROR, "Memory allocation failed, can't divide interval");

	if (*factor == 0.0)
		elog(ERROR, "interval_div:  divide by 0.0 error");

	months = (span1->month / *factor);
	result->month = rint(months);
	result->time = JROUND(span1->time / *factor);
	/* evaluate fractional months as 30 days */
	result->time += JROUND((months - result->month) * 30 * 86400);

	return result;
}	/* interval_div() */

/* timestamp_age()
 * Calculate time difference while retaining year/month fields.
 * Note that this does not result in an accurate absolute time span
 *	since year and month are out of context once the arithmetic
 *	is done.
 */
Interval   *
timestamp_age(Timestamp *timestamp1, Timestamp *timestamp2)
{
	Interval   *result;

	Timestamp	dt1,
				dt2;
	double		fsec,
				fsec1,
				fsec2;
	struct tm	tt,
			   *tm = &tt;
	struct tm	tt1,
			   *tm1 = &tt1;
	struct tm	tt2,
			   *tm2 = &tt2;

	if (!PointerIsValid(timestamp1) || !PointerIsValid(timestamp2))
		return NULL;

	result = palloc(sizeof(Interval));

	dt1 = *timestamp1;
	dt2 = *timestamp2;

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	if (TIMESTAMP_IS_INVALID(dt1)
		|| TIMESTAMP_IS_INVALID(dt2))
	{
		TIMESTAMP_INVALID(result->time);

	}
	else if ((timestamp2tm(dt1, NULL, tm1, &fsec1, NULL) == 0)
			 && (timestamp2tm(dt2, NULL, tm2, &fsec2, NULL) == 0))
	{
		fsec = (fsec1 - fsec2);
		tm->tm_sec = (tm1->tm_sec - tm2->tm_sec);
		tm->tm_min = (tm1->tm_min - tm2->tm_min);
		tm->tm_hour = (tm1->tm_hour - tm2->tm_hour);
		tm->tm_mday = (tm1->tm_mday - tm2->tm_mday);
		tm->tm_mon = (tm1->tm_mon - tm2->tm_mon);
		tm->tm_year = (tm1->tm_year - tm2->tm_year);

		/* flip sign if necessary... */
		if (dt1 < dt2)
		{
			fsec = -fsec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		if (tm->tm_sec < 0)
		{
			tm->tm_sec += 60;
			tm->tm_min--;
		}

		if (tm->tm_min < 0)
		{
			tm->tm_min += 60;
			tm->tm_hour--;
		}

		if (tm->tm_hour < 0)
		{
			tm->tm_hour += 24;
			tm->tm_mday--;
		}

		if (tm->tm_mday < 0)
		{
			if (dt1 < dt2)
			{
				tm->tm_mday += day_tab[isleap(tm1->tm_year)][tm1->tm_mon - 1];
				tm->tm_mon--;
			}
			else
			{
				tm->tm_mday += day_tab[isleap(tm2->tm_year)][tm2->tm_mon - 1];
				tm->tm_mon--;
			}
		}

		if (tm->tm_mon < 0)
		{
			tm->tm_mon += 12;
			tm->tm_year--;
		}

		/* recover sign if necessary... */
		if (dt1 < dt2)
		{
			fsec = -fsec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		if (tm2interval(tm, fsec, result) != 0)
			elog(ERROR, "Unable to decode timestamp");

	}
	else
		elog(ERROR, "Unable to decode timestamp");

	return result;
}	/* timestamp_age() */


/*----------------------------------------------------------
 *	Conversion operators.
 *---------------------------------------------------------*/


/* timestamp_text()
 * Convert timestamp to text data type.
 */
text *
timestamp_text(Timestamp *timestamp)
{
	text	   *result;
	char	   *str;
	int			len;

	if (!PointerIsValid(timestamp))
		return NULL;

	str = timestamp_out(timestamp);

	if (!PointerIsValid(str))
		return NULL;

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARSIZE(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	return result;
}	/* timestamp_text() */


/* text_timestamp()
 * Convert text string to timestamp.
 * Text type is not null terminated, so use temporary string
 *	then call the standard input routine.
 */
Timestamp  *
text_timestamp(text *str)
{
	Timestamp  *result;
	int			i;
	char	   *sp,
			   *dp,
				dstr[MAXDATELEN + 1];

	if (!PointerIsValid(str))
		return NULL;

	sp = VARDATA(str);
	dp = dstr;
	for (i = 0; i < (VARSIZE(str) - VARHDRSZ); i++)
		*dp++ = *sp++;
	*dp = '\0';

	result = timestamp_in(dstr);

	return result;
}	/* text_timestamp() */


/* interval_text()
 * Convert interval to text data type.
 */
text *
interval_text(Interval *interval)
{
	text	   *result;
	char	   *str;
	int			len;

	if (!PointerIsValid(interval))
		return NULL;

	str = interval_out(interval);

	if (!PointerIsValid(str))
		return NULL;

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARSIZE(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	return result;
}	/* interval_text() */


/* text_interval()
 * Convert text string to interval.
 * Text type may not be null terminated, so copy to temporary string
 *	then call the standard input routine.
 */
Interval   *
text_interval(text *str)
{
	Interval   *result;
	int			i;
	char	   *sp,
			   *dp,
				dstr[MAXDATELEN + 1];

	if (!PointerIsValid(str))
		return NULL;

	sp = VARDATA(str);
	dp = dstr;
	for (i = 0; i < (VARSIZE(str) - VARHDRSZ); i++)
		*dp++ = *sp++;
	*dp = '\0';

	result = interval_in(dstr);

	return result;
}	/* text_interval() */

/* timestamp_trunc()
 * Extract specified field from timestamp.
 */
Timestamp  *
timestamp_trunc(text *units, Timestamp *timestamp)
{
	Timestamp  *result;

	Timestamp	dt;
	int			tz;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];
	double		fsec;
	char	   *tzn;
	struct tm	tt,
			   *tm = &tt;

	if ((!PointerIsValid(units)) || (!PointerIsValid(timestamp)))
		return NULL;

	result = palloc(sizeof(Timestamp));

	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(*timestamp))
	{
#if NOT_USED
/* should return null but Postgres doesn't like that currently. - tgl 97/06/12 */
		elog(ERROR, "Timestamp is not finite", NULL);
#endif
		*result = 0;

	}
	else
	{
		dt = (TIMESTAMP_IS_RELATIVE(*timestamp) ? SetTimestamp(*timestamp) : *timestamp);

		if ((type == UNITS) && (timestamp2tm(dt, &tz, tm, &fsec, &tzn) == 0))
		{
			switch (val)
			{
				case DTK_MILLENNIUM:
					tm->tm_year = (tm->tm_year / 1000) * 1000;
				case DTK_CENTURY:
					tm->tm_year = (tm->tm_year / 100) * 100;
				case DTK_DECADE:
					tm->tm_year = (tm->tm_year / 10) * 10;
				case DTK_YEAR:
					tm->tm_mon = 1;
				case DTK_QUARTER:
					tm->tm_mon = (3 * (tm->tm_mon / 4)) + 1;
				case DTK_MONTH:
					tm->tm_mday = 1;
				case DTK_DAY:
					tm->tm_hour = 0;
				case DTK_HOUR:
					tm->tm_min = 0;
				case DTK_MINUTE:
					tm->tm_sec = 0;
				case DTK_SECOND:
					fsec = 0;
					break;

				case DTK_MILLISEC:
					fsec = rint(fsec * 1000) / 1000;
					break;

				case DTK_MICROSEC:
					fsec = rint(fsec * 1000000) / 1000000;
					break;

				default:
					elog(ERROR, "Timestamp units '%s' not supported", lowunits);
					result = NULL;
			}

			if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
			{
#ifdef USE_POSIX_TIME
				tm->tm_isdst = -1;
				tm->tm_year -= 1900;
				tm->tm_mon -= 1;
				tm->tm_isdst = -1;
				mktime(tm);
				tm->tm_year += 1900;
				tm->tm_mon += 1;

#if defined(HAVE_TM_ZONE)
				tz = -(tm->tm_gmtoff);	/* tm_gmtoff is Sun/DEC-ism */
#elif defined(HAVE_INT_TIMEZONE)

#ifdef __CYGWIN__
				tz = (tm->tm_isdst ? (_timezone - 3600) : _timezone);
#else
				tz = (tm->tm_isdst ? (timezone - 3600) : timezone);
#endif

#else
#error USE_POSIX_TIME is defined but neither HAVE_TM_ZONE or HAVE_INT_TIMEZONE are defined
#endif

#else							/* !USE_POSIX_TIME */
				tz = CTimeZone;
#endif
			}
			else
			{
				tm->tm_isdst = 0;
				tz = 0;
			}

			if (tm2timestamp(tm, fsec, &tz, result) != 0)
				elog(ERROR, "Unable to truncate timestamp to '%s'", lowunits);

#if NOT_USED
		}
		else if ((type == RESERV) && (val == DTK_EPOCH))
		{
			TIMESTAMP_EPOCH(*result);
			*result = dt - SetTimestamp(*result);
#endif

		}
		else
		{
			elog(ERROR, "Timestamp units '%s' not recognized", lowunits);
			result = NULL;
		}
	}

	return result;
}	/* timestamp_trunc() */

/* interval_trunc()
 * Extract specified field from interval.
 */
Interval   *
interval_trunc(text *units, Interval *interval)
{
	Interval   *result;

	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];
	double		fsec;
	struct tm	tt,
			   *tm = &tt;

	if ((!PointerIsValid(units)) || (!PointerIsValid(interval)))
		return NULL;

	result = palloc(sizeof(Interval));

	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);

	if (INTERVAL_IS_INVALID(*interval))
	{
#if NOT_USED
		elog(ERROR, "Interval is not finite", NULL);
#endif
		result = NULL;

	}
	else if (type == UNITS)
	{

		if (interval2tm(*interval, tm, &fsec) == 0)
		{
			switch (val)
			{
				case DTK_MILLENNIUM:
					tm->tm_year = (tm->tm_year / 1000) * 1000;
				case DTK_CENTURY:
					tm->tm_year = (tm->tm_year / 100) * 100;
				case DTK_DECADE:
					tm->tm_year = (tm->tm_year / 10) * 10;
				case DTK_YEAR:
					tm->tm_mon = 0;
				case DTK_QUARTER:
					tm->tm_mon = (3 * (tm->tm_mon / 4));
				case DTK_MONTH:
					tm->tm_mday = 0;
				case DTK_DAY:
					tm->tm_hour = 0;
				case DTK_HOUR:
					tm->tm_min = 0;
				case DTK_MINUTE:
					tm->tm_sec = 0;
				case DTK_SECOND:
					fsec = 0;
					break;

				case DTK_MILLISEC:
					fsec = rint(fsec * 1000) / 1000;
					break;

				case DTK_MICROSEC:
					fsec = rint(fsec * 1000000) / 1000000;
					break;

				default:
					elog(ERROR, "Interval units '%s' not supported", lowunits);
					result = NULL;
			}

			if (tm2interval(tm, fsec, result) != 0)
				elog(ERROR, "Unable to truncate interval to '%s'", lowunits);

		}
		else
		{
			elog(NOTICE, "Interval out of range");
			result = NULL;
		}

#if NOT_USED
	}
	else if ((type == RESERV) && (val == DTK_EPOCH))
	{
		*result = interval->time;
		if (interval->month != 0)
		{
			*result += ((365.25 * 86400) * (interval->month / 12));
			*result += ((30 * 86400) * (interval->month % 12));
		}
#endif

	}
	else
	{
		elog(ERROR, "Interval units '%s' not recognized", textout(units));
		result = NULL;
	}

	return result;
}	/* interval_trunc() */


/* timestamp_part()
 * Extract specified field from timestamp.
 */
float64
timestamp_part(text *units, Timestamp *timestamp)
{
	float64		result;

	Timestamp	dt;
	int			tz;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];
	double		dummy;
	double		fsec;
	char	   *tzn;
	struct tm	tt,
			   *tm = &tt;

	if ((!PointerIsValid(units)) || (!PointerIsValid(timestamp)))
		return NULL;

	result = palloc(sizeof(float64data));

	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);
	if (type == IGNORE)
		type = DecodeSpecial(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(*timestamp))
	{
#if NOT_USED
/* should return null but Postgres doesn't like that currently. - tgl 97/06/12 */
		elog(ERROR, "Timestamp is not finite", NULL);
#endif
		*result = 0;

	}
	else
	{
		dt = (TIMESTAMP_IS_RELATIVE(*timestamp) ? SetTimestamp(*timestamp) : *timestamp);

		if ((type == UNITS) && (timestamp2tm(dt, &tz, tm, &fsec, &tzn) == 0))
		{
			switch (val)
			{
				case DTK_TZ:
					*result = tz;
					break;

				case DTK_TZ_MINUTE:
					*result = tz / 60;
					TMODULO(*result, dummy, 60e0);
					break;

				case DTK_TZ_HOUR:
					dummy = tz;
					TMODULO(dummy, *result, 3600e0);
					break;

				case DTK_MICROSEC:
					*result = (fsec * 1000000);
					break;

				case DTK_MILLISEC:
					*result = (fsec * 1000);
					break;

				case DTK_SECOND:
					*result = (tm->tm_sec + fsec);
					break;

				case DTK_MINUTE:
					*result = tm->tm_min;
					break;

				case DTK_HOUR:
					*result = tm->tm_hour;
					break;

				case DTK_DAY:
					*result = tm->tm_mday;
					break;

				case DTK_MONTH:
					*result = tm->tm_mon;
					break;

				case DTK_QUARTER:
					*result = (tm->tm_mon / 4) + 1;
					break;

				case DTK_WEEK:
					{
						int day0, day4, dayn;
						dayn = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
						day4 = date2j(tm->tm_year, 1, 4);
						/* day0 == offset to first day of week (Monday) */
						day0 = (j2day(day4 - 1) % 7);
						/* We need the first week containing a Thursday,
						 * otherwise this day falls into the previous year
						 * for purposes of counting weeks
						 */
						if (dayn < (day4 - day0))
						{
							day4 = date2j((tm->tm_year - 1), 1, 4);
							/* day0 == offset to first day of week (Monday) */
							day0 = (j2day(day4 - 1) % 7);
						}
						*result = (((dayn - (day4 - day0)) / 7) + 1);
						/* Sometimes the last few days in a year will fall into
						 * the first week of the next year, so check for this.
						 */
						if (*result >= 53)
						{
							day4 = date2j((tm->tm_year + 1), 1, 4);
							/* day0 == offset to first day of week (Monday) */
							day0 = (j2day(day4 - 1) % 7);
							if (dayn >= (day4 - day0))
								*result = (((dayn - (day4 - day0)) / 7) + 1);
						}
					}
					break;

				case DTK_YEAR:
					*result = tm->tm_year;
					break;

				case DTK_DECADE:
					*result = (tm->tm_year / 10);
					break;

				case DTK_CENTURY:
					*result = (tm->tm_year / 100);
					break;

				case DTK_MILLENNIUM:
					*result = (tm->tm_year / 1000);
					break;

				default:
					elog(ERROR, "Timestamp units '%s' not supported", lowunits);
					*result = 0;
			}

		}
		else if (type == RESERV)
		{
			switch (val)
			{
				case DTK_EPOCH:
					TIMESTAMP_EPOCH(*result);
					*result = dt - SetTimestamp(*result);
					break;

				case DTK_DOW:
					if (timestamp2tm(dt, &tz, tm, &fsec, &tzn) != 0)
						elog(ERROR, "Unable to encode timestamp");

					*result = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
					break;

				case DTK_DOY:
					if (timestamp2tm(dt, &tz, tm, &fsec, &tzn) != 0)
						elog(ERROR, "Unable to encode timestamp");

					*result = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
							   - date2j(tm->tm_year, 1, 1) + 1);
					break;

				default:
					elog(ERROR, "Timestamp units '%s' not supported", lowunits);
					*result = 0;
			}

		}
		else
		{
			elog(ERROR, "Timestamp units '%s' not recognized", lowunits);
			*result = 0;
		}
	}

	return result;
}	/* timestamp_part() */


/* interval_part()
 * Extract specified field from interval.
 */
float64
interval_part(text *units, Interval *interval)
{
	float64		result;

	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];
	double		fsec;
	struct tm	tt,
			   *tm = &tt;

	if ((!PointerIsValid(units)) || (!PointerIsValid(interval)))
		return NULL;

	result = palloc(sizeof(float64data));

	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);
	if (type == IGNORE)
		type = DecodeSpecial(0, lowunits, &val);

	if (INTERVAL_IS_INVALID(*interval))
	{
#if NOT_USED
		elog(ERROR, "Interval is not finite");
#endif
		*result = 0;

	}
	else if (type == UNITS)
	{

		if (interval2tm(*interval, tm, &fsec) == 0)
		{
			switch (val)
			{
				case DTK_MICROSEC:
					*result = (fsec * 1000000);
					break;

				case DTK_MILLISEC:
					*result = (fsec * 1000);
					break;

				case DTK_SECOND:
					*result = (tm->tm_sec + fsec);
					break;

				case DTK_MINUTE:
					*result = tm->tm_min;
					break;

				case DTK_HOUR:
					*result = tm->tm_hour;
					break;

				case DTK_DAY:
					*result = tm->tm_mday;
					break;

				case DTK_MONTH:
					*result = tm->tm_mon;
					break;

				case DTK_QUARTER:
					*result = (tm->tm_mon / 4) + 1;
					break;

				case DTK_YEAR:
					*result = tm->tm_year;
					break;

				case DTK_DECADE:
					*result = (tm->tm_year / 10);
					break;

				case DTK_CENTURY:
					*result = (tm->tm_year / 100);
					break;

				case DTK_MILLENNIUM:
					*result = (tm->tm_year / 1000);
					break;

				default:
					elog(ERROR, "Interval units '%s' not yet supported", textout(units));
					result = NULL;
			}

		}
		else
		{
			elog(NOTICE, "Interval out of range");
			*result = 0;
		}

	}
	else if ((type == RESERV) && (val == DTK_EPOCH))
	{
		*result = interval->time;
		if (interval->month != 0)
		{
			*result += ((365.25 * 86400) * (interval->month / 12));
			*result += ((30 * 86400) * (interval->month % 12));
		}

	}
	else
	{
		elog(ERROR, "Interval units '%s' not recognized", textout(units));
		*result = 0;
	}

	return result;
}	/* interval_part() */


/* timestamp_zone()
 * Encode timestamp type with specified time zone.
 */
text *
timestamp_zone(text *zone, Timestamp *timestamp)
{
	text	   *result;

	Timestamp	dt;
	int			tz;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowzone[MAXDATELEN + 1];
	char	   *tzn,
				upzone[MAXDATELEN + 1];
	double		fsec;
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];
	int			len;

	if ((!PointerIsValid(zone)) || (!PointerIsValid(timestamp)))
		return NULL;

	up = VARDATA(zone);
	lp = lowzone;
	for (i = 0; i < (VARSIZE(zone) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeSpecial(0, lowzone, &val);

	if (TIMESTAMP_NOT_FINITE(*timestamp))
	{

		/*
		 * could return null but Postgres doesn't like that currently. -
		 * tgl 97/06/12
		 */
		elog(ERROR, "Timestamp is not finite");
		result = NULL;

	}
	else if ((type == TZ) || (type == DTZ))
	{
		tm->tm_isdst = ((type == DTZ) ? 1 : 0);
		tz = val * 60;

		dt = (TIMESTAMP_IS_RELATIVE(*timestamp) ? SetTimestamp(*timestamp) : *timestamp);
		dt = dt2local(dt, tz);

		if (timestamp2tm(dt, NULL, tm, &fsec, NULL) != 0)
			elog(ERROR, "Timestamp not legal");

		up = upzone;
		lp = lowzone;
		for (i = 0; *lp != '\0'; i++)
			*up++ = toupper(*lp++);
		*up = '\0';

		tzn = upzone;
		EncodeDateTime(tm, fsec, &tz, &tzn, DateStyle, buf);

		len = (strlen(buf) + VARHDRSZ);

		result = palloc(len);

		VARSIZE(result) = len;
		memmove(VARDATA(result), buf, (len - VARHDRSZ));

	}
	else
	{
		elog(ERROR, "Time zone '%s' not recognized", lowzone);
		result = NULL;
	}

	return result;
}	/* timestamp_zone() */
