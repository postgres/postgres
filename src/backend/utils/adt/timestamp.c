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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/timestamp.c,v 1.37 2000/11/06 15:57:00 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <sys/types.h>
#include <float.h>
#include <limits.h>

#include "access/hash.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"


static double time2t(const int hour, const int min, const double sec);
static int	EncodeSpecialTimestamp(Timestamp dt, char *str);
static Timestamp dt2local(Timestamp dt, int timezone);
static void dt2time(Timestamp dt, int *hour, int *min, double *sec);
static int	interval2tm(Interval span, struct tm * tm, float8 *fsec);
static int	tm2interval(struct tm * tm, double fsec, Interval *span);


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/* timestamp_in()
 * Convert a string to internal form.
 */
Datum
timestamp_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Timestamp	result;
	double		fsec;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	int			dtype;
	int			nf;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + 1];

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
	  || (DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz) != 0))
		elog(ERROR, "Bad timestamp external representation '%s'", str);

	switch (dtype)
	{
		case DTK_DATE:
			if (tm2timestamp(tm, fsec, &tz, &result) != 0)
				elog(ERROR, "Timestamp out of range '%s'", str);
			break;

		case DTK_EPOCH:
			TIMESTAMP_EPOCH(result);
			break;

		case DTK_CURRENT:
			TIMESTAMP_CURRENT(result);
			break;

		case DTK_LATE:
			TIMESTAMP_NOEND(result);
			break;

		case DTK_EARLY:
			TIMESTAMP_NOBEGIN(result);
			break;

		case DTK_INVALID:
			TIMESTAMP_INVALID(result);
			break;

		default:
			elog(ERROR, "Internal coding error, can't input timestamp '%s'", str);
			TIMESTAMP_INVALID(result); /* keep compiler quiet */
	}

	PG_RETURN_TIMESTAMP(result);
}

/* timestamp_out()
 * Convert a timestamp to external form.
 */
Datum
timestamp_out(PG_FUNCTION_ARGS)
{
	Timestamp	dt = PG_GETARG_TIMESTAMP(0);
	char	   *result;
	int			tz;
	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	char	   *tzn;
	char		buf[MAXDATELEN + 1];

	if (TIMESTAMP_IS_RESERVED(dt))
		EncodeSpecialTimestamp(dt, buf);
	else if (timestamp2tm(dt, &tz, tm, &fsec, &tzn) == 0)
		EncodeDateTime(tm, fsec, &tz, &tzn, DateStyle, buf);
	else
		EncodeSpecialTimestamp(DT_INVALID, buf);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}


/* interval_in()
 * Convert a string to internal form.
 *
 * External format(s):
 *	Uses the generic date/time parsing and decoding routines.
 */
Datum
interval_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
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

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
		|| (DecodeDateDelta(field, ftype, nf, &dtype, tm, &fsec) != 0))
		elog(ERROR, "Bad interval external representation '%s'", str);

	span = (Interval *) palloc(sizeof(Interval));

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

	PG_RETURN_INTERVAL_P(span);
}

/* interval_out()
 * Convert a time span to external form.
 */
Datum
interval_out(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	char	   *result;
	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	char		buf[MAXDATELEN + 1];

	if (interval2tm(*span, tm, &fsec) != 0)
		PG_RETURN_NULL();

	if (EncodeTimeSpan(tm, fsec, DateStyle, buf) != 0)
		elog(ERROR, "Unable to format interval");

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}


/* EncodeSpecialTimestamp()
 * Convert reserved timestamp data type to string.
 */
static int
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

Datum
now(PG_FUNCTION_ARGS)
{
	Timestamp	result;
	AbsoluteTime sec;

	sec = GetCurrentTransactionStartTime();

	result = (sec - ((date2j(2000, 1, 1) - date2j(1970, 1, 1)) * 86400));

	PG_RETURN_TIMESTAMP(result);
}

static void
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

#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
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

#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
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

# if defined(HAVE_TM_ZONE)
			tm->tm_gmtoff = tx->tm_gmtoff;
			tm->tm_zone = tx->tm_zone;

			*tzp = -(tm->tm_gmtoff);	/* tm_gmtoff is Sun/DEC-ism */
			if (tzn != NULL)
				*tzn = (char *) tm->tm_zone;
# elif defined(HAVE_INT_TIMEZONE)
#  ifdef __CYGWIN__
			*tzp = (tm->tm_isdst ? (_timezone - 3600) : _timezone);
#  else
			*tzp = (tm->tm_isdst ? (timezone - 3600) : timezone);
#  endif
			if (tzn != NULL)
				*tzn = tzname[(tm->tm_isdst > 0)];
# endif

#else /* not (HAVE_TM_ZONE || HAVE_INT_TIMEZONE) */
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
static int
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

static int
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

static Timestamp
dt2local(Timestamp dt, int tz)
{
	dt -= tz;
	dt = JROUND(dt);
	return dt;
}	/* dt2local() */


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/


Datum
timestamp_finite(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);

	PG_RETURN_BOOL(! TIMESTAMP_NOT_FINITE(timestamp));
}

Datum
interval_finite(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);

	PG_RETURN_BOOL(! INTERVAL_NOT_FINITE(*interval));
}


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

/*
 *		timestamp_relop - is timestamp1 relop timestamp2
 */
Datum
timestamp_eq(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	if (TIMESTAMP_IS_INVALID(dt1) || TIMESTAMP_IS_INVALID(dt2))
		PG_RETURN_BOOL(false);

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	PG_RETURN_BOOL(dt1 == dt2);
}

Datum
timestamp_ne(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	if (TIMESTAMP_IS_INVALID(dt1) || TIMESTAMP_IS_INVALID(dt2))
		PG_RETURN_BOOL(false);

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	PG_RETURN_BOOL(dt1 != dt2);
}

Datum
timestamp_lt(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	if (TIMESTAMP_IS_INVALID(dt1) || TIMESTAMP_IS_INVALID(dt2))
		PG_RETURN_BOOL(false);

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	PG_RETURN_BOOL(dt1 < dt2);
}

Datum
timestamp_gt(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	if (TIMESTAMP_IS_INVALID(dt1) || TIMESTAMP_IS_INVALID(dt2))
		PG_RETURN_BOOL(false);

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	PG_RETURN_BOOL(dt1 > dt2);
}

Datum
timestamp_le(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	if (TIMESTAMP_IS_INVALID(dt1) || TIMESTAMP_IS_INVALID(dt2))
		PG_RETURN_BOOL(false);

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	PG_RETURN_BOOL(dt1 <= dt2);
}

Datum
timestamp_ge(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	if (TIMESTAMP_IS_INVALID(dt1) || TIMESTAMP_IS_INVALID(dt2))
		PG_RETURN_BOOL(false);

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	PG_RETURN_BOOL(dt1 >= dt2);
}


/*		timestamp_cmp	- 3-state comparison for timestamp
 *		collate invalid timestamp at the end
 */
Datum
timestamp_cmp(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	if (TIMESTAMP_IS_INVALID(dt1))
	{
		PG_RETURN_INT32(TIMESTAMP_IS_INVALID(dt2) ? 0 : 1);
	}
	else if (TIMESTAMP_IS_INVALID(dt2))
	{
		PG_RETURN_INT32(-1);
	}
	else
	{
		if (TIMESTAMP_IS_RELATIVE(dt1))
			dt1 = SetTimestamp(dt1);
		if (TIMESTAMP_IS_RELATIVE(dt2))
			dt2 = SetTimestamp(dt2);
	}

	PG_RETURN_INT32((dt1 < dt2) ? -1 : ((dt1 > dt2) ? 1 : 0));
}


/*
 *		interval_relop	- is interval1 relop interval2
 */
Datum
interval_eq(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	if (INTERVAL_IS_INVALID(*interval1) || INTERVAL_IS_INVALID(*interval2))
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL((interval1->time == interval2->time) &&
				   (interval1->month == interval2->month));
}

Datum
interval_ne(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	if (INTERVAL_IS_INVALID(*interval1) || INTERVAL_IS_INVALID(*interval2))
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL((interval1->time != interval2->time) ||
				   (interval1->month != interval2->month));
}

Datum
interval_lt(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);
	double		span1,
				span2;

	if (INTERVAL_IS_INVALID(*interval1) || INTERVAL_IS_INVALID(*interval2))
		PG_RETURN_BOOL(false);

	span1 = interval1->time;
	if (interval1->month != 0)
		span1 += (interval1->month * (30.0 * 86400));
	span2 = interval2->time;
	if (interval2->month != 0)
		span2 += (interval2->month * (30.0 * 86400));

	PG_RETURN_BOOL(span1 < span2);
}

Datum
interval_gt(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);
	double		span1,
				span2;

	if (INTERVAL_IS_INVALID(*interval1) || INTERVAL_IS_INVALID(*interval2))
		PG_RETURN_BOOL(false);

	span1 = interval1->time;
	if (interval1->month != 0)
		span1 += (interval1->month * (30.0 * 86400));
	span2 = interval2->time;
	if (interval2->month != 0)
		span2 += (interval2->month * (30.0 * 86400));

	PG_RETURN_BOOL(span1 > span2);
}

Datum
interval_le(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);
	double		span1,
				span2;

	if (INTERVAL_IS_INVALID(*interval1) || INTERVAL_IS_INVALID(*interval2))
		PG_RETURN_BOOL(false);

	span1 = interval1->time;
	if (interval1->month != 0)
		span1 += (interval1->month * (30.0 * 86400));
	span2 = interval2->time;
	if (interval2->month != 0)
		span2 += (interval2->month * (30.0 * 86400));

	PG_RETURN_BOOL(span1 <= span2);
}

Datum
interval_ge(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);
	double		span1,
				span2;

	if (INTERVAL_IS_INVALID(*interval1) || INTERVAL_IS_INVALID(*interval2))
		PG_RETURN_BOOL(false);

	span1 = interval1->time;
	if (interval1->month != 0)
		span1 += (interval1->month * (30.0 * 86400));
	span2 = interval2->time;
	if (interval2->month != 0)
		span2 += (interval2->month * (30.0 * 86400));

	PG_RETURN_BOOL(span1 >= span2);
}


/*		interval_cmp	- 3-state comparison for interval
 */
Datum
interval_cmp(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);
	double		span1,
				span2;

	if (INTERVAL_IS_INVALID(*interval1))
		PG_RETURN_INT32(INTERVAL_IS_INVALID(*interval2) ? 0 : 1);
	else if (INTERVAL_IS_INVALID(*interval2))
		PG_RETURN_INT32(-1);

	span1 = interval1->time;
	if (interval1->month != 0)
		span1 += (interval1->month * (30.0 * 86400));
	span2 = interval2->time;
	if (interval2->month != 0)
		span2 += (interval2->month * (30.0 * 86400));

	PG_RETURN_INT32((span1 < span2) ? -1 : (span1 > span2) ? 1 : 0);
}

/*
 * interval, being an unusual size, needs a specialized hash function.
 */
Datum
interval_hash(PG_FUNCTION_ARGS)
{
	Interval   *key = PG_GETARG_INTERVAL_P(0);

	/*
	 * Specify hash length as sizeof(double) + sizeof(int4), not as
	 * sizeof(Interval), so that any garbage pad bytes in the structure
	 * won't be included in the hash!
	 */
	return hash_any((char *) key, sizeof(double) + sizeof(int4));
}

/* overlaps_timestamp()
 * Implements the SQL92 OVERLAPS operator.
 * Algorithm from Date and Darwen, 1997
 */
Datum
overlaps_timestamp(PG_FUNCTION_ARGS)
{
	/* The arguments are Timestamps, but we leave them as generic Datums
	 * to avoid unnecessary conversions between value and reference forms...
	 */
	Datum		ts1 = PG_GETARG_DATUM(0);
	Datum		te1 = PG_GETARG_DATUM(1);
	Datum		ts2 = PG_GETARG_DATUM(2);
	Datum		te2 = PG_GETARG_DATUM(3);

#define TIMESTAMP_GT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timestamp_gt,t1,t2))
#define TIMESTAMP_LT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timestamp_lt,t1,t2))
#define TIMESTAMP_EQ(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timestamp_eq,t1,t2))

	/* Make sure we have ordered pairs... */
	if (TIMESTAMP_GT(ts1, te1))
	{
		Datum		tt = ts1;

		ts1 = te1;
		te1 = tt;
	}
	if (TIMESTAMP_GT(ts2, te2))
	{
		Datum		tt = ts2;

		ts2 = te2;
		te2 = tt;
	}

	PG_RETURN_BOOL((TIMESTAMP_GT(ts1, ts2) &&
					(TIMESTAMP_LT(ts1, te2) || TIMESTAMP_LT(te1, te2))) ||
				   (TIMESTAMP_GT(ts2, ts1) &&
					(TIMESTAMP_LT(ts2, te1) || TIMESTAMP_LT(te2, te1))) ||
				   TIMESTAMP_EQ(ts1, ts2));

#undef TIMESTAMP_GT
#undef TIMESTAMP_LT
#undef TIMESTAMP_EQ
}


/*----------------------------------------------------------
 *	"Arithmetic" operators on date/times.
 *---------------------------------------------------------*/

Datum
timestamp_smaller(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	if (TIMESTAMP_IS_INVALID(dt1))
		result = dt2;
	else if (TIMESTAMP_IS_INVALID(dt2))
		result = dt1;
	else
		result = ((dt2 < dt1) ? dt2 : dt1);

	PG_RETURN_TIMESTAMP(result);
}

Datum
timestamp_larger(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	if (TIMESTAMP_IS_INVALID(dt1))
		result = dt2;
	else if (TIMESTAMP_IS_INVALID(dt2))
		result = dt1;
	else
		result = ((dt2 > dt1) ? dt2 : dt1);

	PG_RETURN_TIMESTAMP(result);
}


Datum
timestamp_mi(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	if (TIMESTAMP_IS_RELATIVE(dt1))
		dt1 = SetTimestamp(dt1);
	if (TIMESTAMP_IS_RELATIVE(dt2))
		dt2 = SetTimestamp(dt2);

	if (TIMESTAMP_IS_INVALID(dt1)
		|| TIMESTAMP_IS_INVALID(dt2))
		TIMESTAMP_INVALID(result->time);
	else
		result->time = JROUND(dt1 - dt2);
	result->month = 0;

	PG_RETURN_INTERVAL_P(result);
}


/* timestamp_pl_span()
 * Add a interval to a timestamp data type.
 * Note that interval has provisions for qualitative year/month
 *	units, so try to do the right thing with them.
 * To add a month, increment the month, and use the same day of month.
 * Then, if the next month has fewer days, set the day of month
 *	to the last day of month.
 * Lastly, add in the "quantitative time".
 */
Datum
timestamp_pl_span(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	Timestamp	result;
	Timestamp	dt;
	int			tz;
	char	   *tzn;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else if (INTERVAL_IS_INVALID(*span))
		TIMESTAMP_INVALID(result);
	else
	{
		dt = (TIMESTAMP_IS_RELATIVE(timestamp) ? SetTimestamp(timestamp) : timestamp);

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

				if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
				{
#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
					tm->tm_isdst = -1;
					tm->tm_year -= 1900;
					tm->tm_mon -= 1;
					tm->tm_isdst = -1;
					mktime(tm);
					tm->tm_year += 1900;
					tm->tm_mon += 1;

# if defined(HAVE_TM_ZONE)
					tz = -(tm->tm_gmtoff);	/* tm_gmtoff is Sun/DEC-ism */
# elif defined(HAVE_INT_TIMEZONE)

#  ifdef __CYGWIN__
					tz = (tm->tm_isdst ? (_timezone - 3600) : _timezone);
#  else
					tz = (tm->tm_isdst ? (timezone - 3600) : timezone);
#  endif

# endif

#else /* not (HAVE_TM_ZONE || HAVE_INT_TIMEZONE) */
					tz = CTimeZone;
#endif
				}
				else
				{
					tm->tm_isdst = 0;
					tz = 0;
				}

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

		result = dt;
	}

	PG_RETURN_TIMESTAMP(result);
}

Datum
timestamp_mi_span(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	Interval	tspan;

	tspan.month = - span->month;
	tspan.time = - span->time;

	return DirectFunctionCall2(timestamp_pl_span,
							   TimestampGetDatum(timestamp),
							   PointerGetDatum(&tspan));
}


Datum
interval_um(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->time = -(interval->time);
	result->month = -(interval->month);

	PG_RETURN_INTERVAL_P(result);
}


Datum
interval_smaller(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;
	double		span1,
				span2;

	result = (Interval *) palloc(sizeof(Interval));

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

	PG_RETURN_INTERVAL_P(result);
}

Datum
interval_larger(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;
	double		span1,
				span2;

	result = (Interval *) palloc(sizeof(Interval));

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

	PG_RETURN_INTERVAL_P(result);
}

Datum
interval_pl(PG_FUNCTION_ARGS)
{
	Interval   *span1 = PG_GETARG_INTERVAL_P(0);
	Interval   *span2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->month = (span1->month + span2->month);
	result->time = JROUND(span1->time + span2->time);

	PG_RETURN_INTERVAL_P(result);
}

Datum
interval_mi(PG_FUNCTION_ARGS)
{
	Interval   *span1 = PG_GETARG_INTERVAL_P(0);
	Interval   *span2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->month = (span1->month - span2->month);
	result->time = JROUND(span1->time - span2->time);

	PG_RETURN_INTERVAL_P(result);
}

Datum
interval_mul(PG_FUNCTION_ARGS)
{
	Interval   *span1 = PG_GETARG_INTERVAL_P(0);
	float8		factor = PG_GETARG_FLOAT8(1);
	Interval   *result;
	double		months;

	result = (Interval *) palloc(sizeof(Interval));

	months = (span1->month * factor);
	result->month = rint(months);
	result->time = JROUND(span1->time * factor);
	/* evaluate fractional months as 30 days */
	result->time += JROUND((months - result->month) * 30 * 86400);

	PG_RETURN_INTERVAL_P(result);
}

Datum
mul_d_interval(PG_FUNCTION_ARGS)
{
	/* Args are float8 and Interval *, but leave them as generic Datum */
	Datum		factor = PG_GETARG_DATUM(0);
	Datum		span1 = PG_GETARG_DATUM(1);

	return DirectFunctionCall2(interval_mul, span1, factor);
}

Datum
interval_div(PG_FUNCTION_ARGS)
{
	Interval   *span1 = PG_GETARG_INTERVAL_P(0);
	float8		factor = PG_GETARG_FLOAT8(1);
	Interval   *result;
	double		months;

	result = (Interval *) palloc(sizeof(Interval));

	if (factor == 0.0)
		elog(ERROR, "interval_div:  divide by 0.0 error");

	months = (span1->month / factor);
	result->month = rint(months);
	result->time = JROUND(span1->time / factor);
	/* evaluate fractional months as 30 days */
	result->time += JROUND((months - result->month) * 30 * 86400);

	PG_RETURN_INTERVAL_P(result);
}

/*
 * interval_accum and interval_avg implement the AVG(interval) aggregate.
 *
 * The transition datatype for this aggregate is a 2-element array of
 * intervals, where the first is the running sum and the second contains
 * the number of values so far in its 'time' field.  This is a bit ugly
 * but it beats inventing a specialized datatype for the purpose.
 */

Datum
interval_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Interval   *newval = PG_GETARG_INTERVAL_P(1);
	Datum	   *transdatums;
	int			ndatums;
	Interval	sumX,
				N;
	Interval   *newsum;
	ArrayType  *result;

	/* We assume the input is array of interval */
	deconstruct_array(transarray,
					  false, 12, 'd',
					  &transdatums, &ndatums);
	if (ndatums != 2)
		elog(ERROR, "interval_accum: expected 2-element interval array");
	/*
	 * XXX memcpy, instead of just extracting a pointer, to work around
	 * buggy array code: it won't ensure proper alignment of Interval
	 * objects on machines where double requires 8-byte alignment.
	 * That should be fixed, but in the meantime...
	 */
	memcpy(&sumX, DatumGetIntervalP(transdatums[0]), sizeof(Interval));
	memcpy(&N, DatumGetIntervalP(transdatums[1]), sizeof(Interval));

	newsum = DatumGetIntervalP(DirectFunctionCall2(interval_pl,
												   IntervalPGetDatum(&sumX),
												   IntervalPGetDatum(newval)));
	N.time += 1;

	transdatums[0] = IntervalPGetDatum(newsum);
	transdatums[1] = IntervalPGetDatum(&N);

	result = construct_array(transdatums, 2,
							 false, 12, 'd');

	PG_RETURN_ARRAYTYPE_P(result);
}

Datum
interval_avg(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Datum	   *transdatums;
	int			ndatums;
	Interval	sumX,
				N;

	/* We assume the input is array of interval */
	deconstruct_array(transarray,
					  false, 12, 'd',
					  &transdatums, &ndatums);
	if (ndatums != 2)
		elog(ERROR, "interval_avg: expected 2-element interval array");
	/*
	 * XXX memcpy, instead of just extracting a pointer, to work around
	 * buggy array code: it won't ensure proper alignment of Interval
	 * objects on machines where double requires 8-byte alignment.
	 * That should be fixed, but in the meantime...
	 */
	memcpy(&sumX, DatumGetIntervalP(transdatums[0]), sizeof(Interval));
	memcpy(&N, DatumGetIntervalP(transdatums[1]), sizeof(Interval));

	/* SQL92 defines AVG of no values to be NULL */
	if (N.time == 0)
		PG_RETURN_NULL();

	return DirectFunctionCall2(interval_div,
							   IntervalPGetDatum(&sumX),
							   Float8GetDatum(N.time));
}


/* timestamp_age()
 * Calculate time difference while retaining year/month fields.
 * Note that this does not result in an accurate absolute time span
 *	since year and month are out of context once the arithmetic
 *	is done.
 */
Datum
timestamp_age(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Interval   *result;
	double		fsec,
				fsec1,
				fsec2;
	struct tm	tt,
			   *tm = &tt;
	struct tm	tt1,
			   *tm1 = &tt1;
	struct tm	tt2,
			   *tm2 = &tt2;

	result = (Interval *) palloc(sizeof(Interval));

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

	PG_RETURN_INTERVAL_P(result);
}


/*----------------------------------------------------------
 *	Conversion operators.
 *---------------------------------------------------------*/


/* timestamp_text()
 * Convert timestamp to text data type.
 */
Datum
timestamp_text(PG_FUNCTION_ARGS)
{
	/* Input is a Timestamp, but may as well leave it in Datum form */
	Datum		timestamp = PG_GETARG_DATUM(0);
	text	   *result;
	char	   *str;
	int			len;

	str = DatumGetCString(DirectFunctionCall1(timestamp_out, timestamp));

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARATT_SIZEP(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	PG_RETURN_TEXT_P(result);
}


/* text_timestamp()
 * Convert text string to timestamp.
 * Text type is not null terminated, so use temporary string
 *	then call the standard input routine.
 */
Datum
text_timestamp(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	int			i;
	char	   *sp,
			   *dp,
				dstr[MAXDATELEN + 1];

	if (VARSIZE(str) - VARHDRSZ > MAXDATELEN)
		elog(ERROR, "Bad timestamp external representation (too long)");

	sp = VARDATA(str);
	dp = dstr;
	for (i = 0; i < (VARSIZE(str) - VARHDRSZ); i++)
		*dp++ = *sp++;
	*dp = '\0';

	return DirectFunctionCall1(timestamp_in,
							   CStringGetDatum(dstr));
}


/* interval_text()
 * Convert interval to text data type.
 */
Datum
interval_text(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	text	   *result;
	char	   *str;
	int			len;

	str = DatumGetCString(DirectFunctionCall1(interval_out,
											  IntervalPGetDatum(interval)));

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARATT_SIZEP(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	PG_RETURN_TEXT_P(result);
}


/* text_interval()
 * Convert text string to interval.
 * Text type may not be null terminated, so copy to temporary string
 *	then call the standard input routine.
 */
Datum
text_interval(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	int			i;
	char	   *sp,
			   *dp,
				dstr[MAXDATELEN + 1];

	if (VARSIZE(str) - VARHDRSZ > MAXDATELEN)
		elog(ERROR, "Bad interval external representation (too long)");
	sp = VARDATA(str);
	dp = dstr;
	for (i = 0; i < (VARSIZE(str) - VARHDRSZ); i++)
		*dp++ = *sp++;
	*dp = '\0';

	return DirectFunctionCall1(interval_in, CStringGetDatum(dstr));
}

/* timestamp_trunc()
 * Extract specified field from timestamp.
 */
Datum
timestamp_trunc(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_P(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;
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

	if (VARSIZE(units) - VARHDRSZ > MAXDATELEN)
		elog(ERROR, "Interval units '%s' not recognized",
			 DatumGetCString(DirectFunctionCall1(textout,
												 PointerGetDatum(units))));
	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		PG_RETURN_NULL();
	}
	else
	{
		dt = (TIMESTAMP_IS_RELATIVE(timestamp) ? SetTimestamp(timestamp) : timestamp);

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
					result = 0;
			}

			if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
			{
#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
				tm->tm_isdst = -1;
				tm->tm_year -= 1900;
				tm->tm_mon -= 1;
				tm->tm_isdst = -1;
				mktime(tm);
				tm->tm_year += 1900;
				tm->tm_mon += 1;

# if defined(HAVE_TM_ZONE)
				tz = -(tm->tm_gmtoff);	/* tm_gmtoff is Sun/DEC-ism */
# elif defined(HAVE_INT_TIMEZONE)

#  ifdef __CYGWIN__
				tz = (tm->tm_isdst ? (_timezone - 3600) : _timezone);
#  else
				tz = (tm->tm_isdst ? (timezone - 3600) : timezone);
#  endif

# endif

#else /* not (HAVE_TM_ZONE || HAVE_INT_TIMEZONE) */
				tz = CTimeZone;
#endif
			}
			else
			{
				tm->tm_isdst = 0;
				tz = 0;
			}

			if (tm2timestamp(tm, fsec, &tz, &result) != 0)
				elog(ERROR, "Unable to truncate timestamp to '%s'", lowunits);
		}
#if NOT_USED
		else if ((type == RESERV) && (val == DTK_EPOCH))
		{
			TIMESTAMP_EPOCH(result);
			result = dt - SetTimestamp(result);
		}
#endif
		else
		{
			elog(ERROR, "Timestamp units '%s' not recognized", lowunits);
			result = 0;
		}
	}

	PG_RETURN_TIMESTAMP(result);
}

/* interval_trunc()
 * Extract specified field from interval.
 */
Datum
interval_trunc(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_P(0);
	Interval   *interval = PG_GETARG_INTERVAL_P(1);
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

	result = (Interval *) palloc(sizeof(Interval));

	if (VARSIZE(units) - VARHDRSZ > MAXDATELEN)
		elog(ERROR, "Interval units '%s' not recognized",
			 DatumGetCString(DirectFunctionCall1(textout,
												 PointerGetDatum(units))));
	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);

	if (INTERVAL_IS_INVALID(*interval))
	{
#if NOT_USED
		elog(ERROR, "Interval is not finite");
#endif
		PG_RETURN_NULL();
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
					PG_RETURN_NULL();
			}

			if (tm2interval(tm, fsec, result) != 0)
				elog(ERROR, "Unable to truncate interval to '%s'", lowunits);

		}
		else
		{
			elog(NOTICE, "Interval out of range");
			PG_RETURN_NULL();
		}

	}
#if NOT_USED
	else if ((type == RESERV) && (val == DTK_EPOCH))
	{
		*result = interval->time;
		if (interval->month != 0)
		{
			*result += ((365.25 * 86400) * (interval->month / 12));
			*result += ((30 * 86400) * (interval->month % 12));
		}
	}
#endif
	else
	{
		elog(ERROR, "Interval units '%s' not recognized",
			 DatumGetCString(DirectFunctionCall1(textout,
												 PointerGetDatum(units))));
		PG_RETURN_NULL();
	}

	PG_RETURN_INTERVAL_P(result);
}

/* isoweek2date()
 *
 * 	Convert ISO week of year number to date. An year must be already set.  
 *	karel 2000/08/07
 */
void
isoweek2date( int woy, int *year, int *mon, int *mday)
{
	int     day0, day4, dayn;
	
	if (!*year)
		elog(ERROR, "isoweek2date(): can't convert without year information");

	/* fourth day of current year */
	day4 = date2j(*year, 1, 4);
	
	/* day0 == offset to first day of week (Monday) */
	day0 = (j2day(day4 - 1) % 7);

	dayn = ((woy - 1) * 7) + (day4 - day0);
	
	j2date(dayn, year, mon, mday);
}

/* date2isoweek()
 * 
 *	Returns ISO week number of year.
 */
int
date2isoweek(int year, int mon, int mday) 
{
	float8	result;
	int	day0, day4, dayn;
	
	/* current day */	
	dayn = date2j(year, mon, mday);
	
	/* fourth day of current year */
	day4 = date2j(year, 1, 4);
	
	/* day0 == offset to first day of week (Monday) */
	day0 = (j2day(day4 - 1) % 7);
	
	/* We need the first week containing a Thursday,
	 * otherwise this day falls into the previous year
	 * for purposes of counting weeks
	 */
	if (dayn < (day4 - day0))
	{
		day4 = date2j((year - 1), 1, 4);
	
		/* day0 == offset to first day of week (Monday) */
		day0 = (j2day(day4 - 1) % 7);
	}
	
	result = (((dayn - (day4 - day0)) / 7) + 1);
	
	/* Sometimes the last few days in a year will fall into
	 * the first week of the next year, so check for this.
	 */
	if (result >= 53)
	{
		day4 = date2j((year + 1), 1, 4);
	
		/* day0 == offset to first day of week (Monday) */
		day0 = (j2day(day4 - 1) % 7);
		
		if (dayn >= (day4 - day0))
			result = (((dayn - (day4 - day0)) / 7) + 1);
	}

	return (int) result;
} 
 
 
/* timestamp_part()
 * Extract specified field from timestamp.
 */
Datum
timestamp_part(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_P(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	float8		result;
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

	if (VARSIZE(units) - VARHDRSZ > MAXDATELEN)
		elog(ERROR, "Interval units '%s' not recognized",
			 DatumGetCString(DirectFunctionCall1(textout,
												 PointerGetDatum(units))));
	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);
	if (type == IGNORE)
		type = DecodeSpecial(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		PG_RETURN_NULL();
	}
	else
	{
		dt = (TIMESTAMP_IS_RELATIVE(timestamp) ? SetTimestamp(timestamp) : timestamp);

		if ((type == UNITS) && (timestamp2tm(dt, &tz, tm, &fsec, &tzn) == 0))
		{
			switch (val)
			{
				case DTK_TZ:
					result = tz;
					break;

				case DTK_TZ_MINUTE:
					result = tz / 60;
					TMODULO(result, dummy, 60e0);
					break;

				case DTK_TZ_HOUR:
					dummy = tz;
					TMODULO(dummy, result, 3600e0);
					break;

				case DTK_MICROSEC:
					result = (fsec * 1000000);
					break;

				case DTK_MILLISEC:
					result = (fsec * 1000);
					break;

				case DTK_SECOND:
					result = (tm->tm_sec + fsec);
					break;

				case DTK_MINUTE:
					result = tm->tm_min;
					break;

				case DTK_HOUR:
					result = tm->tm_hour;
					break;

				case DTK_DAY:
					result = tm->tm_mday;
					break;

				case DTK_MONTH:
					result = tm->tm_mon;
					break;

				case DTK_QUARTER:
					result = (tm->tm_mon / 4) + 1;
					break;

				case DTK_WEEK:
					result = (float8) date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);
					break;

				case DTK_YEAR:
					result = tm->tm_year;
					break;

				case DTK_DECADE:
					result = (tm->tm_year / 10);
					break;

				case DTK_CENTURY:
					result = (tm->tm_year / 100);
					break;

				case DTK_MILLENNIUM:
					result = (tm->tm_year / 1000);
					break;

				default:
					elog(ERROR, "Timestamp units '%s' not supported", lowunits);
					result = 0;
			}

		}
		else if (type == RESERV)
		{
			switch (val)
			{
				case DTK_EPOCH:
					TIMESTAMP_EPOCH(result);
					result = dt - SetTimestamp(result);
					break;

				case DTK_DOW:
					if (timestamp2tm(dt, &tz, tm, &fsec, &tzn) != 0)
						elog(ERROR, "Unable to encode timestamp");

					result = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
					break;

				case DTK_DOY:
					if (timestamp2tm(dt, &tz, tm, &fsec, &tzn) != 0)
						elog(ERROR, "Unable to encode timestamp");

					result = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
							   - date2j(tm->tm_year, 1, 1) + 1);
					break;

				default:
					elog(ERROR, "Timestamp units '%s' not supported", lowunits);
					result = 0;
			}

		}
		else
		{
			elog(ERROR, "Timestamp units '%s' not recognized", lowunits);
			result = 0;
		}
	}

	PG_RETURN_FLOAT8(result);
}


/* interval_part()
 * Extract specified field from interval.
 */
Datum
interval_part(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_P(0);
	Interval   *interval = PG_GETARG_INTERVAL_P(1);
	float8		result;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];
	double		fsec;
	struct tm	tt,
			   *tm = &tt;

	if (VARSIZE(units) - VARHDRSZ > MAXDATELEN)
		elog(ERROR, "Interval units '%s' not recognized",
			 DatumGetCString(DirectFunctionCall1(textout,
												 PointerGetDatum(units))));
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
		result = 0;
	}
	else if (type == UNITS)
	{
		if (interval2tm(*interval, tm, &fsec) == 0)
		{
			switch (val)
			{
				case DTK_MICROSEC:
					result = (fsec * 1000000);
					break;

				case DTK_MILLISEC:
					result = (fsec * 1000);
					break;

				case DTK_SECOND:
					result = (tm->tm_sec + fsec);
					break;

				case DTK_MINUTE:
					result = tm->tm_min;
					break;

				case DTK_HOUR:
					result = tm->tm_hour;
					break;

				case DTK_DAY:
					result = tm->tm_mday;
					break;

				case DTK_MONTH:
					result = tm->tm_mon;
					break;

				case DTK_QUARTER:
					result = (tm->tm_mon / 4) + 1;
					break;

				case DTK_YEAR:
					result = tm->tm_year;
					break;

				case DTK_DECADE:
					result = (tm->tm_year / 10);
					break;

				case DTK_CENTURY:
					result = (tm->tm_year / 100);
					break;

				case DTK_MILLENNIUM:
					result = (tm->tm_year / 1000);
					break;

				default:
					elog(ERROR, "Interval units '%s' not yet supported",
						 DatumGetCString(DirectFunctionCall1(textout,
													PointerGetDatum(units))));
					result = 0;
			}

		}
		else
		{
			elog(NOTICE, "Interval out of range");
			result = 0;
		}
	}
	else if ((type == RESERV) && (val == DTK_EPOCH))
	{
		result = interval->time;
		if (interval->month != 0)
		{
			result += ((365.25 * 86400) * (interval->month / 12));
			result += ((30 * 86400) * (interval->month % 12));
		}
	}
	else
	{
		elog(ERROR, "Interval units '%s' not recognized",
			 DatumGetCString(DirectFunctionCall1(textout,
												 PointerGetDatum(units))));
		result = 0;
	}

	PG_RETURN_FLOAT8(result);
}


/* timestamp_zone()
 * Encode timestamp type with specified time zone.
 */
Datum
timestamp_zone(PG_FUNCTION_ARGS)
{
	text	   *zone = PG_GETARG_TEXT_P(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
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

	if (VARSIZE(zone) - VARHDRSZ > MAXDATELEN)
		elog(ERROR, "Time zone '%s' not recognized",
			 DatumGetCString(DirectFunctionCall1(textout,
												 PointerGetDatum(zone))));
	up = VARDATA(zone);
	lp = lowzone;
	for (i = 0; i < (VARSIZE(zone) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeSpecial(0, lowzone, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		PG_RETURN_NULL();
	}
	else if ((type == TZ) || (type == DTZ))
	{
		tm->tm_isdst = ((type == DTZ) ? 1 : 0);
		tz = val * 60;

		dt = (TIMESTAMP_IS_RELATIVE(timestamp) ? SetTimestamp(timestamp) : timestamp);
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

		VARATT_SIZEP(result) = len;
		memmove(VARDATA(result), buf, (len - VARHDRSZ));
	}
	else
	{
		elog(ERROR, "Time zone '%s' not recognized", lowzone);
		result = NULL;
	}

	PG_RETURN_TEXT_P(result);
} /* timestamp_zone() */

/* timestamp_izone()
 * Encode timestamp type with specified time interval as time zone.
 * Require ISO-formatted result, since character-string time zone is not available.
 */
Datum
timestamp_izone(PG_FUNCTION_ARGS)
{
	Interval   *zone = PG_GETARG_INTERVAL_P(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	text	   *result;
	Timestamp	dt;
	int			tz;
	char	   *tzn = "";
	double		fsec;
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];
	int			len;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_NULL();

	if (zone->month != 0)
		elog(ERROR, "INTERVAL time zone not legal (month specified)");

	tm->tm_isdst = -1;
	tz = -(zone->time);

	dt = (TIMESTAMP_IS_RELATIVE(timestamp) ? SetTimestamp(timestamp) : timestamp);
	dt = dt2local(dt, tz);

	if (timestamp2tm(dt, NULL, tm, &fsec, NULL) != 0)
		elog(ERROR, "Timestamp not legal");

	EncodeDateTime(tm, fsec, &tz, &tzn, USE_ISO_DATES, buf);
	len = (strlen(buf) + VARHDRSZ);

	result = palloc(len);
	VARATT_SIZEP(result) = len;
	memmove(VARDATA(result), buf, (len - VARHDRSZ));

	PG_RETURN_TEXT_P(result);
} /* timestamp_izone() */
