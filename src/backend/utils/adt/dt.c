/*-------------------------------------------------------------------------
 *
 * dt.c--
 *	  Functions for the built-in type "dt".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/dt.c,v 1.50 1998/01/07 18:46:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>

#include "postgres.h"
#include "miscadmin.h"
#ifdef HAVE_FLOAT_H
#include <float.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#ifndef USE_POSIX_TIME
#include <sys/timeb.h>
#endif
#include "utils/builtins.h"

static int DecodeDate(char *str, int fmask, int *tmask, struct tm * tm);
static int
DecodeNumber(int flen, char *field,
			 int fmask, int *tmask, struct tm * tm, double *fsec);
static int
DecodeNumberField(int len, char *str,
				  int fmask, int *tmask, struct tm * tm, double *fsec);
static int DecodeSpecial(int field, char *lowtoken, int *val);
static int
DecodeTime(char *str, int fmask, int *tmask,
		   struct tm * tm, double *fsec);
static int DecodeTimezone(char *str, int *tzp);
static int DecodeUnits(int field, char *lowtoken, int *val);
static int EncodeSpecialDateTime(DateTime dt, char *str);
static datetkn *datebsearch(char *key, datetkn *base, unsigned int nel);
static DateTime dt2local(DateTime dt, int timezone);
static void dt2time(DateTime dt, int *hour, int *min, double *sec);
static int j2day(int jd);

#define USE_DATE_CACHE 1
#define ROUND_ALL 0

#define isleap(y) (((y % 4) == 0) && (((y % 100) != 0) || ((y % 400) == 0)))

int			mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0};

char	   *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
"Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL};

char	   *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
"Thursday", "Friday", "Saturday", NULL};

/* TMODULO()
 * Macro to replace modf(), which is broken on some platforms.
 */
#define TMODULO(t,q,u) {q = ((t < 0)? ceil(t / u): floor(t / u)); \
						if (q != 0) t -= rint(q * u);}

static void GetEpochTime(struct tm * tm);

#define UTIME_MINYEAR (1901)
#define UTIME_MINMONTH (12)
#define UTIME_MINDAY (14)
#define UTIME_MAXYEAR (2038)
#define UTIME_MAXMONTH (01)
#define UTIME_MAXDAY (18)

#define IS_VALID_UTIME(y,m,d) (((y > UTIME_MINYEAR) \
 || ((y == UTIME_MINYEAR) && ((m > UTIME_MINMONTH) \
  || ((m == UTIME_MINMONTH) && (d >= UTIME_MINDAY))))) \
 && ((y < UTIME_MAXYEAR) \
 || ((y == UTIME_MAXYEAR) && ((m < UTIME_MAXMONTH) \
  || ((m == UTIME_MAXMONTH) && (d <= UTIME_MAXDAY))))))


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/* datetime_in()
 * Convert a string to internal form.
 */
DateTime   *
datetime_in(char *str)
{
	DateTime   *result;

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
		elog(ERROR, "Bad (null) datetime external representation", NULL);

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
	  || (DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz) != 0))
		elog(ERROR, "Bad datetime external representation '%s'", str);

	result = palloc(sizeof(DateTime));

	switch (dtype)
	{
		case DTK_DATE:
			if (tm2datetime(tm, fsec, &tz, result) != 0)
				elog(ERROR, "Datetime out of range '%s'", str);

#ifdef DATEDEBUG
			printf("datetime_in- date is %f\n", *result);
#endif

			break;

		case DTK_EPOCH:
			DATETIME_EPOCH(*result);
			break;

		case DTK_CURRENT:
			DATETIME_CURRENT(*result);
			break;

		case DTK_LATE:
			DATETIME_NOEND(*result);
			break;

		case DTK_EARLY:
			DATETIME_NOBEGIN(*result);
			break;

		case DTK_INVALID:
			DATETIME_INVALID(*result);
			break;

		default:
			elog(ERROR, "Internal coding error, can't input datetime '%s'", str);
	}

	return (result);
} /* datetime_in() */

/* datetime_out()
 * Convert a datetime to external form.
 */
char	   *
datetime_out(DateTime *dt)
{
	char	   *result;
	int			tz;
	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	char	   *tzn;
	char		buf[MAXDATELEN + 1];

	if (!PointerIsValid(dt))
		return (NULL);

	if (DATETIME_IS_RESERVED(*dt))
	{
		EncodeSpecialDateTime(*dt, buf);

	}
	else if (datetime2tm(*dt, &tz, tm, &fsec, &tzn) == 0)
	{
		EncodeDateTime(tm, fsec, &tz, &tzn, DateStyle, buf);

	}
	else
	{
		EncodeSpecialDateTime(DT_INVALID, buf);
	}

	result = palloc(strlen(buf) + 1);

	strcpy(result, buf);

	return (result);
} /* datetime_out() */


/* timespan_in()
 * Convert a string to internal form.
 *
 * External format(s):
 *	Uses the generic date/time parsing and decoding routines.
 */
TimeSpan   *
timespan_in(char *str)
{
	TimeSpan   *span;

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
		elog(ERROR, "Bad (null) timespan external representation", NULL);

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
		|| (DecodeDateDelta(field, ftype, nf, &dtype, tm, &fsec) != 0))
		elog(ERROR, "Bad timespan external representation '%s'", str);

	span = palloc(sizeof(TimeSpan));

	switch (dtype)
	{
		case DTK_DELTA:
			if (tm2timespan(tm, fsec, span) != 0)
			{
#if FALSE
				TIMESPAN_INVALID(span);
#endif
				elog(ERROR, "Bad timespan external representation '%s'", str);
			}
			break;

		default:
			elog(ERROR, "Internal coding error, can't input timespan '%s'", str);
	}

	return (span);
} /* timespan_in() */

/* timespan_out()
 * Convert a time span to external form.
 */
char	   *
timespan_out(TimeSpan *span)
{
	char	   *result;

	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	char		buf[MAXDATELEN + 1];

	if (!PointerIsValid(span))
		return (NULL);

	if (timespan2tm(*span, tm, &fsec) != 0)
		return (NULL);

	if (EncodeTimeSpan(tm, fsec, DateStyle, buf) != 0)
		elog(ERROR, "Unable to format timespan", NULL);

	result = palloc(strlen(buf) + 1);

	strcpy(result, buf);
	return (result);
} /* timespan_out() */


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/


bool
datetime_finite(DateTime *datetime)
{
	if (!PointerIsValid(datetime))
		return FALSE;

	return (!DATETIME_NOT_FINITE(*datetime));
} /* datetime_finite() */

bool
timespan_finite(TimeSpan *timespan)
{
	if (!PointerIsValid(timespan))
		return FALSE;

	return (!TIMESPAN_NOT_FINITE(*timespan));
} /* timespan_finite() */


/*----------------------------------------------------------
 *	Relational operators for datetime.
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

#ifdef DATEDEBUG
	printf("GetEpochTime- %04d-%02d-%02d %02d:%02d:%02d\n",
		   tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif

	return;
} /* GetEpochTime() */

DateTime
SetDateTime(DateTime dt)
{
	struct tm	tt;

	if (DATETIME_IS_CURRENT(dt))
	{
		GetCurrentTime(&tt);
		tm2datetime(&tt, 0, NULL, &dt);
		dt = dt2local(dt, -CTimeZone);

#ifdef DATEDEBUG
		printf("SetDateTime- current time is %f\n", dt);
#endif
	}
	else
	{							/* if (DATETIME_IS_EPOCH(dt1)) */
		GetEpochTime(&tt);
		tm2datetime(&tt, 0, NULL, &dt);
#ifdef DATEDEBUG
		printf("SetDateTime- epoch time is %f\n", dt);
#endif
	}

	return (dt);
} /* SetDateTime() */

/*		datetime_relop	- is datetime1 relop datetime2
 */
bool
datetime_eq(DateTime *datetime1, DateTime *datetime2)
{
	DateTime	dt1,
				dt2;

	if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
		return FALSE;

	dt1 = *datetime1;
	dt2 = *datetime2;

	if (DATETIME_IS_INVALID(dt1) || DATETIME_IS_INVALID(dt2))
		return FALSE;

	if (DATETIME_IS_RELATIVE(dt1))
		dt1 = SetDateTime(dt1);
	if (DATETIME_IS_RELATIVE(dt2))
		dt2 = SetDateTime(dt2);

	return (dt1 == dt2);
} /* datetime_eq() */

bool
datetime_ne(DateTime *datetime1, DateTime *datetime2)
{
	DateTime	dt1,
				dt2;

	if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
		return FALSE;

	dt1 = *datetime1;
	dt2 = *datetime2;

	if (DATETIME_IS_INVALID(dt1) || DATETIME_IS_INVALID(dt2))
		return FALSE;

	if (DATETIME_IS_RELATIVE(dt1))
		dt1 = SetDateTime(dt1);
	if (DATETIME_IS_RELATIVE(dt2))
		dt2 = SetDateTime(dt2);

	return (dt1 != dt2);
} /* datetime_ne() */

bool
datetime_lt(DateTime *datetime1, DateTime *datetime2)
{
	DateTime	dt1,
				dt2;

	if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
		return FALSE;

	dt1 = *datetime1;
	dt2 = *datetime2;

	if (DATETIME_IS_INVALID(dt1) || DATETIME_IS_INVALID(dt2))
		return FALSE;

	if (DATETIME_IS_RELATIVE(dt1))
		dt1 = SetDateTime(dt1);
	if (DATETIME_IS_RELATIVE(dt2))
		dt2 = SetDateTime(dt2);

	return (dt1 < dt2);
} /* datetime_lt() */

bool
datetime_gt(DateTime *datetime1, DateTime *datetime2)
{
	DateTime	dt1,
				dt2;

	if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
		return FALSE;

	dt1 = *datetime1;
	dt2 = *datetime2;

	if (DATETIME_IS_INVALID(dt1) || DATETIME_IS_INVALID(dt2))
		return FALSE;

	if (DATETIME_IS_RELATIVE(dt1))
		dt1 = SetDateTime(dt1);
	if (DATETIME_IS_RELATIVE(dt2))
		dt2 = SetDateTime(dt2);

#ifdef DATEDEBUG
	printf("datetime_gt- %f %s greater than %f\n", dt1, ((dt1 > dt2) ? "is" : "is not"), dt2);
#endif
	return (dt1 > dt2);
} /* datetime_gt() */

bool
datetime_le(DateTime *datetime1, DateTime *datetime2)
{
	DateTime	dt1,
				dt2;

	if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
		return FALSE;

	dt1 = *datetime1;
	dt2 = *datetime2;

	if (DATETIME_IS_INVALID(dt1) || DATETIME_IS_INVALID(dt2))
		return FALSE;

	if (DATETIME_IS_RELATIVE(dt1))
		dt1 = SetDateTime(dt1);
	if (DATETIME_IS_RELATIVE(dt2))
		dt2 = SetDateTime(dt2);

	return (dt1 <= dt2);
} /* datetime_le() */

bool
datetime_ge(DateTime *datetime1, DateTime *datetime2)
{
	DateTime	dt1,
				dt2;

	if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
		return FALSE;

	dt1 = *datetime1;
	dt2 = *datetime2;

	if (DATETIME_IS_INVALID(dt1) || DATETIME_IS_INVALID(dt2))
		return FALSE;

	if (DATETIME_IS_RELATIVE(dt1))
		dt1 = SetDateTime(dt1);
	if (DATETIME_IS_RELATIVE(dt2))
		dt2 = SetDateTime(dt2);

	return (dt1 >= dt2);
} /* datetime_ge() */


/*		datetime_cmp	- 3-state comparison for datetime
 *		collate invalid datetime at the end
 */
int
datetime_cmp(DateTime *datetime1, DateTime *datetime2)
{
	DateTime	dt1,
				dt2;

	if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
		return 0;

	dt1 = *datetime1;
	dt2 = *datetime2;

	if (DATETIME_IS_INVALID(dt1))
	{
		return ((DATETIME_IS_INVALID(dt2) ? 0 : 1));

	}
	else if (DATETIME_IS_INVALID(dt2))
	{
		return (-1);

	}
	else
	{
		if (DATETIME_IS_RELATIVE(dt1))
			dt1 = SetDateTime(dt1);
		if (DATETIME_IS_RELATIVE(dt2))
			dt2 = SetDateTime(dt2);
	}

	return (((dt1 < dt2) ? -1 : ((dt1 > dt2) ? 1 : 0)));
} /* datetime_cmp() */


/*		timespan_relop	- is timespan1 relop timespan2
 */
bool
timespan_eq(TimeSpan *timespan1, TimeSpan *timespan2)
{
	if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
		return FALSE;

	if (TIMESPAN_IS_INVALID(*timespan1) || TIMESPAN_IS_INVALID(*timespan2))
		return FALSE;

	return ((timespan1->time == timespan2->time)
			&& (timespan1->month == timespan2->month));
} /* timespan_eq() */

bool
timespan_ne(TimeSpan *timespan1, TimeSpan *timespan2)
{
	if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
		return FALSE;

	if (TIMESPAN_IS_INVALID(*timespan1) || TIMESPAN_IS_INVALID(*timespan2))
		return FALSE;

	return ((timespan1->time != timespan2->time)
			|| (timespan1->month != timespan2->month));
} /* timespan_ne() */

bool
timespan_lt(TimeSpan *timespan1, TimeSpan *timespan2)
{
	double		span1,
				span2;

	if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
		return FALSE;

	if (TIMESPAN_IS_INVALID(*timespan1) || TIMESPAN_IS_INVALID(*timespan2))
		return FALSE;

	span1 = timespan1->time;
	if (timespan1->month != 0)
		span1 += (timespan1->month * (30.0 * 86400));
	span2 = timespan2->time;
	if (timespan2->month != 0)
		span2 += (timespan2->month * (30.0 * 86400));

	return (span1 < span2);
} /* timespan_lt() */

bool
timespan_gt(TimeSpan *timespan1, TimeSpan *timespan2)
{
	double		span1,
				span2;

	if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
		return FALSE;

	if (TIMESPAN_IS_INVALID(*timespan1) || TIMESPAN_IS_INVALID(*timespan2))
		return FALSE;

	span1 = timespan1->time;
	if (timespan1->month != 0)
		span1 += (timespan1->month * (30.0 * 86400));
	span2 = timespan2->time;
	if (timespan2->month != 0)
		span2 += (timespan2->month * (30.0 * 86400));

	return (span1 > span2);
} /* timespan_gt() */

bool
timespan_le(TimeSpan *timespan1, TimeSpan *timespan2)
{
	double		span1,
				span2;

	if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
		return FALSE;

	if (TIMESPAN_IS_INVALID(*timespan1) || TIMESPAN_IS_INVALID(*timespan2))
		return FALSE;

	span1 = timespan1->time;
	if (timespan1->month != 0)
		span1 += (timespan1->month * (30.0 * 86400));
	span2 = timespan2->time;
	if (timespan2->month != 0)
		span2 += (timespan2->month * (30.0 * 86400));

	return (span1 <= span2);
} /* timespan_le() */

bool
timespan_ge(TimeSpan *timespan1, TimeSpan *timespan2)
{
	double		span1,
				span2;

	if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
		return FALSE;

	if (TIMESPAN_IS_INVALID(*timespan1) || TIMESPAN_IS_INVALID(*timespan2))
		return FALSE;

	span1 = timespan1->time;
	if (timespan1->month != 0)
		span1 += (timespan1->month * (30.0 * 86400));
	span2 = timespan2->time;
	if (timespan2->month != 0)
		span2 += (timespan2->month * (30.0 * 86400));

	return (span1 >= span2);
} /* timespan_ge() */


/*		timespan_cmp	- 3-state comparison for timespan
 */
int
timespan_cmp(TimeSpan *timespan1, TimeSpan *timespan2)
{
	double		span1,
				span2;

	if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
		return 0;

	if (TIMESPAN_IS_INVALID(*timespan1))
	{
		return (TIMESPAN_IS_INVALID(*timespan2) ? 0 : 1);

	}
	else if (TIMESPAN_IS_INVALID(*timespan2))
	{
		return (-1);
	}

	span1 = timespan1->time;
	if (timespan1->month != 0)
		span1 += (timespan1->month * (30.0 * 86400));
	span2 = timespan2->time;
	if (timespan2->month != 0)
		span2 += (timespan2->month * (30.0 * 86400));

	return ((span1 < span2) ? -1 : (span1 > span2) ? 1 : 0);
} /* timespan_cmp() */


/*----------------------------------------------------------
 *	"Arithmetic" operators on date/times.
 *		datetime_foo	returns foo as an object (pointer) that
 *						can be passed between languages.
 *		datetime_xx		is an internal routine which returns the
 *						actual value.
 *---------------------------------------------------------*/

DateTime   *
datetime_smaller(DateTime *datetime1, DateTime *datetime2)
{
	DateTime   *result;

	DateTime	dt1,
				dt2;

	if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
		return NULL;

	dt1 = *datetime1;
	dt2 = *datetime2;

	result = palloc(sizeof(DateTime));

	if (DATETIME_IS_RELATIVE(dt1))
		dt1 = SetDateTime(dt1);
	if (DATETIME_IS_RELATIVE(dt2))
		dt2 = SetDateTime(dt2);

	if (DATETIME_IS_INVALID(dt1))
	{
		*result = dt2;
	}
	else if (DATETIME_IS_INVALID(dt2))
	{
		*result = dt1;
	}
	else
	{
		*result = ((dt2 < dt1) ? dt2 : dt1);
	}

	return (result);
} /* datetime_smaller() */

DateTime   *
datetime_larger(DateTime *datetime1, DateTime *datetime2)
{
	DateTime   *result;

	DateTime	dt1,
				dt2;

	if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
		return NULL;

	dt1 = *datetime1;
	dt2 = *datetime2;

	result = palloc(sizeof(DateTime));

	if (DATETIME_IS_RELATIVE(dt1))
		dt1 = SetDateTime(dt1);
	if (DATETIME_IS_RELATIVE(dt2))
		dt2 = SetDateTime(dt2);

	if (DATETIME_IS_INVALID(dt1))
	{
		*result = dt2;
	}
	else if (DATETIME_IS_INVALID(dt2))
	{
		*result = dt1;
	}
	else
	{
		*result = ((dt2 > dt1) ? dt2 : dt1);
	}

	return (result);
} /* datetime_larger() */


TimeSpan   *
datetime_mi(DateTime *datetime1, DateTime *datetime2)
{
	TimeSpan   *result;

	DateTime	dt1,
				dt2;

	if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
		return NULL;

	dt1 = *datetime1;
	dt2 = *datetime2;

	result = palloc(sizeof(TimeSpan));

	if (DATETIME_IS_RELATIVE(dt1))
		dt1 = SetDateTime(dt1);
	if (DATETIME_IS_RELATIVE(dt2))
		dt2 = SetDateTime(dt2);

#ifdef DATEDEBUG
	printf("datetime_mi- evaluate %f - %f\n", dt1, dt2);
#endif

	if (DATETIME_IS_INVALID(dt1)
		|| DATETIME_IS_INVALID(dt2))
	{
		DATETIME_INVALID(result->time);

	}
	else
	{
		result->time = JROUND(dt1 - dt2);
	}
	result->month = 0;

	return (result);
} /* datetime_mi() */


/* datetime_pl_span()
 * Add a timespan to a datetime data type.
 * Note that timespan has provisions for qualitative year/month
 *	units, so try to do the right thing with them.
 * To add a month, increment the month, and use the same day of month.
 * Then, if the next month has fewer days, set the day of month
 *	to the last day of month.
 */
DateTime   *
datetime_pl_span(DateTime *datetime, TimeSpan *span)
{
	DateTime   *result;
	DateTime	dt;
	int			tz;
	char	   *tzn;

	if ((!PointerIsValid(datetime)) || (!PointerIsValid(span)))
		return NULL;

	result = palloc(sizeof(DateTime));

#ifdef DATEDEBUG
	printf("datetime_pl_span- add %f to %d %f\n", *datetime, span->month, span->time);
#endif

	if (DATETIME_NOT_FINITE(*datetime))
	{
		*result = *datetime;

	}
	else if (TIMESPAN_IS_INVALID(*span))
	{
		DATETIME_INVALID(*result);

	}
	else
	{
		dt = (DATETIME_IS_RELATIVE(*datetime) ? SetDateTime(*datetime) : *datetime);

#ifdef ROUND_ALL
		dt = JROUND(dt + span->time);
#else
		dt += span->time;
#endif

		if (span->month != 0)
		{
			struct tm	tt,
					   *tm = &tt;
			double		fsec;

			if (datetime2tm(dt, &tz, tm, &fsec, &tzn) == 0)
			{
#ifdef DATEDEBUG
				printf("datetime_pl_span- date was %04d-%02d-%02d %02d:%02d:%02d\n",
					   tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif
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
				if (tm->tm_mday > mdays[tm->tm_mon - 1])
				{
					if ((tm->tm_mon == 2) && isleap(tm->tm_year))
					{
						tm->tm_mday = (mdays[tm->tm_mon - 1] + 1);
					}
					else
					{
						tm->tm_mday = mdays[tm->tm_mon - 1];
					}
				}

#ifdef DATEDEBUG
				printf("datetime_pl_span- date becomes %04d-%02d-%02d %02d:%02d:%02d\n",
					   tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif
				if (tm2datetime(tm, fsec, &tz, &dt) != 0)
					elog(ERROR, "Unable to add datetime and timespan", NULL);

			}
			else
			{
				DATETIME_INVALID(dt);
			}
		}

		*result = dt;
	}

	return (result);
} /* datetime_pl_span() */

DateTime   *
datetime_mi_span(DateTime *datetime, TimeSpan *span)
{
	DateTime   *result;
	TimeSpan	tspan;

	if (!PointerIsValid(datetime) || !PointerIsValid(span))
		return NULL;

	tspan.month = -span->month;
	tspan.time = -span->time;

	result = datetime_pl_span(datetime, &tspan);

	return (result);
} /* datetime_mi_span() */


TimeSpan   *
timespan_um(TimeSpan *timespan)
{
	TimeSpan   *result;

	if (!PointerIsValid(timespan))
		return NULL;

	result = palloc(sizeof(TimeSpan));

	result->time = -(timespan->time);
	result->month = -(timespan->month);

	return (result);
} /* timespan_um() */


TimeSpan   *
timespan_smaller(TimeSpan *timespan1, TimeSpan *timespan2)
{
	TimeSpan   *result;

	double		span1,
				span2;

	if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
		return NULL;

	result = palloc(sizeof(TimeSpan));

	if (TIMESPAN_IS_INVALID(*timespan1))
	{
		result->time = timespan2->time;
		result->month = timespan2->month;

	}
	else if (TIMESPAN_IS_INVALID(*timespan2))
	{
		result->time = timespan1->time;
		result->month = timespan1->month;

	}
	else
	{
		span1 = timespan1->time;
		if (timespan1->month != 0)
			span1 += (timespan1->month * (30.0 * 86400));
		span2 = timespan2->time;
		if (timespan2->month != 0)
			span2 += (timespan2->month * (30.0 * 86400));

#ifdef DATEDEBUG
		printf("timespan_smaller- months %d %d times %f %f spans %f %f\n",
			   timespan1->month, timespan2->month, timespan1->time, timespan2->time, span1, span2);
#endif

		if (span2 < span1)
		{
			result->time = timespan2->time;
			result->month = timespan2->month;

		}
		else
		{
			result->time = timespan1->time;
			result->month = timespan1->month;
		}
	}

	return (result);
} /* timespan_smaller() */

TimeSpan   *
timespan_larger(TimeSpan *timespan1, TimeSpan *timespan2)
{
	TimeSpan   *result;

	double		span1,
				span2;

	if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
		return NULL;

	result = palloc(sizeof(TimeSpan));

	if (TIMESPAN_IS_INVALID(*timespan1))
	{
		result->time = timespan2->time;
		result->month = timespan2->month;

	}
	else if (TIMESPAN_IS_INVALID(*timespan2))
	{
		result->time = timespan1->time;
		result->month = timespan1->month;

	}
	else
	{
		span1 = timespan1->time;
		if (timespan1->month != 0)
			span1 += (timespan1->month * (30.0 * 86400));
		span2 = timespan2->time;
		if (timespan2->month != 0)
			span2 += (timespan2->month * (30.0 * 86400));

#ifdef DATEDEBUG
		printf("timespan_larger- months %d %d times %f %f spans %f %f\n",
			   timespan1->month, timespan2->month, timespan1->time, timespan2->time, span1, span2);
#endif

		if (span2 > span1)
		{
			result->time = timespan2->time;
			result->month = timespan2->month;

		}
		else
		{
			result->time = timespan1->time;
			result->month = timespan1->month;
		}
	}

	return (result);
} /* timespan_larger() */


TimeSpan   *
timespan_pl(TimeSpan *span1, TimeSpan *span2)
{
	TimeSpan   *result;

	if ((!PointerIsValid(span1)) || (!PointerIsValid(span2)))
		return NULL;

	result = palloc(sizeof(TimeSpan));

	result->month = (span1->month + span2->month);
	result->time = JROUND(span1->time + span2->time);

	return (result);
} /* timespan_pl() */

TimeSpan   *
timespan_mi(TimeSpan *span1, TimeSpan *span2)
{
	TimeSpan   *result;

	if ((!PointerIsValid(span1)) || (!PointerIsValid(span2)))
		return NULL;

	result = palloc(sizeof(TimeSpan));

	result->month = (span1->month - span2->month);
	result->time = JROUND(span1->time - span2->time);

	return (result);
} /* timespan_mi() */

TimeSpan   *
timespan_div(TimeSpan *span1, float8 *arg2)
{
	TimeSpan   *result;

	if ((!PointerIsValid(span1)) || (!PointerIsValid(arg2)))
		return NULL;

	if (!PointerIsValid(result = palloc(sizeof(TimeSpan))))
		elog(ERROR, "Memory allocation failed, can't subtract timespans", NULL);

	if (*arg2 == 0.0)
		elog(ERROR, "timespan_div:  divide by 0.0 error");

	result->month = rint(span1->month / *arg2);
	result->time = JROUND(span1->time / *arg2);

	return (result);
} /* timespan_div() */

/* datetime_age()
 * Calculate time difference while retaining year/month fields.
 * Note that this does not result in an accurate absolute time span
 *	since year and month are out of context once the arithmetic
 *	is done.
 */
TimeSpan   *
datetime_age(DateTime *datetime1, DateTime *datetime2)
{
	TimeSpan   *result;

	DateTime	dt1,
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

	if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
		return NULL;

	result = palloc(sizeof(TimeSpan));

	dt1 = *datetime1;
	dt2 = *datetime2;

	if (DATETIME_IS_RELATIVE(dt1))
		dt1 = SetDateTime(dt1);
	if (DATETIME_IS_RELATIVE(dt2))
		dt2 = SetDateTime(dt2);

	if (DATETIME_IS_INVALID(dt1)
		|| DATETIME_IS_INVALID(dt2))
	{
		DATETIME_INVALID(result->time);

	}
	else if ((datetime2tm(dt1, NULL, tm1, &fsec1, NULL) == 0)
			 && (datetime2tm(dt2, NULL, tm2, &fsec2, NULL) == 0))
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
				tm->tm_mday += mdays[tm1->tm_mon - 1];
				if (isleap(tm1->tm_year) && (tm1->tm_mon == 2))
					tm->tm_mday++;
				tm->tm_mon--;
			}
			else
			{
				tm->tm_mday += mdays[tm2->tm_mon - 1];
				if (isleap(tm2->tm_year) && (tm2->tm_mon == 2))
					tm->tm_mday++;
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

		if (tm2timespan(tm, fsec, result) != 0)
		{
			elog(ERROR, "Unable to decode datetime", NULL);
		}

#if FALSE
		result->time = (fsec2 - fsec1);
		result->time += (tm2->tm_sec - tm1->tm_sec);
		result->time += 60 * (tm2->tm_min - tm1->tm_min);
		result->time += 3600 * (tm2->tm_hour - tm1->tm_hour);
		result->time += 86400 * (tm2->tm_mday - tm1->tm_mday);

		result->month = 12 * (tm2->tm_year - tm1->tm_year);
		result->month += (tm2->tm_mon - tm1->tm_mon);
#endif

	}
	else
	{
		elog(ERROR, "Unable to decode datetime", NULL);
	}

	return (result);
} /* datetime_age() */


/*----------------------------------------------------------
 *	Conversion operators.
 *---------------------------------------------------------*/


/* datetime_text()
 * Convert datetime to text data type.
 */
text	   *
datetime_text(DateTime *datetime)
{
	text	   *result;
	char	   *str;
	int			len;

	if (!PointerIsValid(datetime))
		return NULL;

	str = datetime_out(datetime);

	if (!PointerIsValid(str))
		return NULL;

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARSIZE(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	return (result);
} /* datetime_text() */


/* text_datetime()
 * Convert text string to datetime.
 * Text type is not null terminated, so use temporary string
 *	then call the standard input routine.
 */
DateTime   *
text_datetime(text *str)
{
	DateTime   *result;
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

	result = datetime_in(dstr);

	return (result);
} /* text_datetime() */


/* timespan_text()
 * Convert timespan to text data type.
 */
text	   *
timespan_text(TimeSpan *timespan)
{
	text	   *result;
	char	   *str;
	int			len;

	if (!PointerIsValid(timespan))
		return NULL;

	str = timespan_out(timespan);

	if (!PointerIsValid(str))
		return NULL;

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARSIZE(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	return (result);
} /* timespan_text() */


/* text_timespan()
 * Convert text string to timespan.
 * Text type may not be null terminated, so copy to temporary string
 *	then call the standard input routine.
 */
TimeSpan *
text_timespan(text *str)
{
	TimeSpan   *result;
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

	result = timespan_in(dstr);

	return (result);
} /* text_timespan() */

/* datetime_trunc()
 * Extract specified field from datetime.
 */
DateTime   *
datetime_trunc(text *units, DateTime *datetime)
{
	DateTime   *result;

	DateTime	dt;
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

	if ((!PointerIsValid(units)) || (!PointerIsValid(datetime)))
		return NULL;

	result = palloc(sizeof(DateTime));

	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);
#if FALSE
	if (type == IGNORE)
	{
		type = DecodeSpecial(0, lowunits, &val);
	}
#endif

#ifdef DATEDEBUG
	if (type == IGNORE)
		strcpy(lowunits, "(unknown)");
	printf("datetime_trunc- units %s type=%d value=%d\n", lowunits, type, val);
#endif

	if (DATETIME_NOT_FINITE(*datetime))
	{
#if FALSE
/* should return null but Postgres doesn't like that currently. - tgl 97/06/12 */
		elog(ERROR, "Datetime is not finite", NULL);
#endif
		*result = 0;

	}
	else
	{
		dt = (DATETIME_IS_RELATIVE(*datetime) ? SetDateTime(*datetime) : *datetime);

		if ((type == UNITS) && (datetime2tm(dt, &tz, tm, &fsec, &tzn) == 0))
		{
			switch (val)
			{
				case DTK_MILLENIUM:
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
					fsec = rint(fsec * 1000) / 1000;
					break;

				default:
					elog(ERROR, "Datetime units '%s' not supported", lowunits);
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

#ifdef HAVE_INT_TIMEZONE
				tz = ((tm->tm_isdst > 0) ? (timezone - 3600) : timezone);

#else							/* !HAVE_INT_TIMEZONE */
				tz = -(tm->tm_gmtoff);	/* tm_gmtoff is Sun/DEC-ism */
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

			if (tm2datetime(tm, fsec, &tz, result) != 0)
				elog(ERROR, "Unable to truncate datetime to '%s'", lowunits);

#if FALSE
		}
		else if ((type == RESERV) && (val == DTK_EPOCH))
		{
			DATETIME_EPOCH(*result);
			*result = dt - SetDateTime(*result);
#endif

		}
		else
		{
			elog(ERROR, "Datetime units '%s' not recognized", lowunits);
			result = NULL;
		}
	}

	return (result);
} /* datetime_trunc() */

/* timespan_trunc()
 * Extract specified field from timespan.
 */
TimeSpan   *
timespan_trunc(text *units, TimeSpan *timespan)
{
	TimeSpan   *result;

	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];
	double		fsec;
	struct tm	tt,
			   *tm = &tt;

	if ((!PointerIsValid(units)) || (!PointerIsValid(timespan)))
		return NULL;

	result = palloc(sizeof(TimeSpan));

	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);
#if FALSE
	if (type == IGNORE)
	{
		type = DecodeSpecial(0, lowunits, &val);
	}
#endif

#ifdef DATEDEBUG
	if (type == IGNORE)
		strcpy(lowunits, "(unknown)");
	printf("timespan_trunc- units %s type=%d value=%d\n", lowunits, type, val);
#endif

	if (TIMESPAN_IS_INVALID(*timespan))
	{
#if FALSE
		elog(ERROR, "Timespan is not finite", NULL);
#endif
		result = NULL;

	}
	else if (type == UNITS)
	{

		if (timespan2tm(*timespan, tm, &fsec) == 0)
		{
			switch (val)
			{
				case DTK_MILLENIUM:
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
					fsec = rint(fsec * 1000) / 1000;
					break;

				default:
					elog(ERROR, "Timespan units '%s' not supported", lowunits);
					result = NULL;
			}

			if (tm2timespan(tm, fsec, result) != 0)
				elog(ERROR, "Unable to truncate timespan to '%s'", lowunits);

		}
		else
		{
			elog(NOTICE, "Timespan out of range", NULL);
			result = NULL;
		}

#if FALSE
	}
	else if ((type == RESERV) && (val == DTK_EPOCH))
	{
		*result = timespan->time;
		if (timespan->month != 0)
		{
			*result += ((365.25 * 86400) * (timespan->month / 12));
			*result += ((30 * 86400) * (timespan->month % 12));
		}
#endif

	}
	else
	{
		elog(ERROR, "Timespan units '%s' not recognized", units);
		result = NULL;
	}

	return (result);
} /* timespan_trunc() */


/* datetime_part()
 * Extract specified field from datetime.
 */
float64
datetime_part(text *units, DateTime *datetime)
{
	float64		result;

	DateTime	dt;
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

	if ((!PointerIsValid(units)) || (!PointerIsValid(datetime)))
		return NULL;

	result = palloc(sizeof(float64data));

	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);
	if (type == IGNORE)
	{
		type = DecodeSpecial(0, lowunits, &val);
	}

#ifdef DATEDEBUG
	if (type == IGNORE)
		strcpy(lowunits, "(unknown)");
	printf("datetime_part- units %s type=%d value=%d\n", lowunits, type, val);
#endif

	if (DATETIME_NOT_FINITE(*datetime))
	{
#if FALSE
/* should return null but Postgres doesn't like that currently. - tgl 97/06/12 */
		elog(ERROR, "Datetime is not finite", NULL);
#endif
		*result = 0;

	}
	else
	{
		dt = (DATETIME_IS_RELATIVE(*datetime) ? SetDateTime(*datetime) : *datetime);

		if ((type == UNITS) && (datetime2tm(dt, &tz, tm, &fsec, &tzn) == 0))
		{
			switch (val)
			{
				case DTK_TZ:
					*result = tz;
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

				case DTK_YEAR:
					*result = tm->tm_year;
					break;

				case DTK_DECADE:
					*result = (tm->tm_year / 10) + 1;
					break;

				case DTK_CENTURY:
					*result = (tm->tm_year / 100) + 1;
					break;

				case DTK_MILLENIUM:
					*result = (tm->tm_year / 1000) + 1;
					break;

				default:
					elog(ERROR, "Datetime units '%s' not supported", lowunits);
					*result = 0;
			}

		}
		else if (type == RESERV)
		{
			switch (val)
			{
				case DTK_EPOCH:
					DATETIME_EPOCH(*result);
					*result = dt - SetDateTime(*result);
					break;

				case DTK_DOW:
					if (datetime2tm(dt, &tz, tm, &fsec, &tzn) != 0)
						elog(ERROR, "Unable to encode datetime", NULL);

					*result = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
					break;

				case DTK_DOY:
					if (datetime2tm(dt, &tz, tm, &fsec, &tzn) != 0)
						elog(ERROR, "Unable to encode datetime", NULL);

					*result = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
						- date2j(tm->tm_year, 1, 1) + 1);
					break;

				default:
					elog(ERROR, "Datetime units '%s' not supported", lowunits);
					*result = 0;
			}

		}
		else
		{
			elog(ERROR, "Datetime units '%s' not recognized", lowunits);
			*result = 0;
		}
	}

	return (result);
} /* datetime_part() */


/* timespan_part()
 * Extract specified field from timespan.
 */
float64
timespan_part(text *units, TimeSpan *timespan)
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

	if ((!PointerIsValid(units)) || (!PointerIsValid(timespan)))
		return NULL;

	result = palloc(sizeof(float64data));

	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);
	if (type == IGNORE)
	{
		type = DecodeSpecial(0, lowunits, &val);
	}

#ifdef DATEDEBUG
	if (type == IGNORE)
		strcpy(lowunits, "(unknown)");
	printf("timespan_part- units %s type=%d value=%d\n", lowunits, type, val);
#endif

	if (TIMESPAN_IS_INVALID(*timespan))
	{
#if FALSE
		elog(ERROR, "Timespan is not finite", NULL);
#endif
		*result = 0;

	}
	else if (type == UNITS)
	{

		if (timespan2tm(*timespan, tm, &fsec) == 0)
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
					*result = (tm->tm_year / 10) + 1;
					break;

				case DTK_CENTURY:
					*result = (tm->tm_year / 100) + 1;
					break;

				case DTK_MILLENIUM:
					*result = (tm->tm_year / 1000) + 1;
					break;

				default:
					elog(ERROR, "Timespan units '%s' not yet supported", units);
					result = NULL;
			}

		}
		else
		{
			elog(NOTICE, "Timespan out of range", NULL);
			*result = 0;
		}

	}
	else if ((type == RESERV) && (val == DTK_EPOCH))
	{
		*result = timespan->time;
		if (timespan->month != 0)
		{
			*result += ((365.25 * 86400) * (timespan->month / 12));
			*result += ((30 * 86400) * (timespan->month % 12));
		}

	}
	else
	{
		elog(ERROR, "Timespan units '%s' not recognized", units);
		*result = 0;
	}

	return (result);
} /* timespan_part() */


/* datetime_zone()
 * Encode datetime type with specified time zone.
 */
text	   *
datetime_zone(text *zone, DateTime *datetime)
{
	text	   *result;

	DateTime	dt;
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

	if ((!PointerIsValid(zone)) || (!PointerIsValid(datetime)))
		return NULL;

	up = VARDATA(zone);
	lp = lowzone;
	for (i = 0; i < (VARSIZE(zone) - VARHDRSZ); i++)
		*lp++ = tolower(*up++);
	*lp = '\0';

	type = DecodeSpecial(0, lowzone, &val);

#ifdef DATEDEBUG
	if (type == IGNORE)
		strcpy(lowzone, "(unknown)");
	printf("datetime_zone- zone %s type=%d value=%d\n", lowzone, type, val);
#endif

	if (DATETIME_NOT_FINITE(*datetime))
	{

		/*
		 * could return null but Postgres doesn't like that currently. -
		 * tgl 97/06/12
		 */
		elog(ERROR, "Datetime is not finite", NULL);
		result = NULL;

	}
	else if ((type == TZ) || (type == DTZ))
	{
		tm->tm_isdst = ((type == DTZ) ? 1 : 0);
		tz = val * 60;

		dt = (DATETIME_IS_RELATIVE(*datetime) ? SetDateTime(*datetime) : *datetime);
		dt = dt2local(dt, tz);

		if (datetime2tm(dt, NULL, tm, &fsec, NULL) != 0)
			elog(ERROR, "Datetime not legal", NULL);

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

	return (result);
} /* datetime_zone() */


/*****************************************************************************
 *	 PRIVATE ROUTINES														 *
 *****************************************************************************/

/* definitions for squeezing values into "value" */
#define ABS_SIGNBIT		(char) 0200
#define VALMASK			(char) 0177
#define NEG(n)			((n)|ABS_SIGNBIT)
#define SIGNEDCHAR(c)	((c)&ABS_SIGNBIT? -((c)&VALMASK): (c))
#define FROMVAL(tp)		(-SIGNEDCHAR((tp)->value) * 10) /* uncompress */
#define TOVAL(tp, v)	((tp)->value = ((v) < 0? NEG((-(v))/10): (v)/10))

/*
 * to keep this table reasonably small, we divide the lexval for TZ and DTZ
 * entries by 10 and truncate the text field at MAXTOKLEN characters.
 * the text field is not guaranteed to be NULL-terminated.
 */
static datetkn datetktbl[] = {
/*		text			token	lexval */
	{EARLY,			RESERV,		DTK_EARLY},		/* "-infinity" reserved for "early time" */
	{"acsst",		DTZ,		63},			/* Cent. Australia */
	{"acst",		TZ,			57},			/* Cent. Australia */
	{DA_D,			ADBC,		AD},			/* "ad" for years >= 0 */
	{"abstime",		IGNORE,		0},				/* "abstime" for pre-v6.1 "Invalid Abstime" */
	{"adt",			DTZ,		NEG(18)},		/* Atlantic Daylight Time */
	{"aesst",		DTZ,		66},			/* E. Australia */
	{"aest",		TZ,			60},			/* Australia Eastern Std Time */
	{"ahst",		TZ,			60},			/* Alaska-Hawaii Std Time */
	{"allballs",	RESERV,		DTK_ZULU},		/* 00:00:00 */
	{"am",			AMPM,		AM},
	{"apr",			MONTH,		4},
	{"april",		MONTH,		4},
	{"ast",			TZ,			NEG(24)},		/* Atlantic Std Time (Canada) */
	{"at",			IGNORE,		0},				/* "at" (throwaway) */
	{"aug",			MONTH,		8},
	{"august",		MONTH,		8},
	{"awsst",		DTZ,		54},			/* W. Australia */
	{"awst",		TZ,			48},			/* W. Australia */
	{DB_C,			ADBC,		BC},			/* "bc" for years < 0 */
	{"bst",			TZ,			6},				/* British Summer Time */
	{"bt",			TZ,			18},			/* Baghdad Time */
	{"cadt",		DTZ,		63},			/* Central Australian DST */
	{"cast",		TZ,			57},			/* Central Australian ST */
	{"cat",			TZ,			NEG(60)},		/* Central Alaska Time */
	{"cct",			TZ,			48},			/* China Coast */
	{"cdt",			DTZ,		NEG(30)},		/* Central Daylight Time */
	{"cet",			TZ,			6},				/* Central European Time */
	{"cetdst",		DTZ,		12},			/* Central European Dayl.Time */
	{"cst",			TZ,			NEG(36)},		/* Central Standard Time */
	{DCURRENT,		RESERV,		DTK_CURRENT},	/* "current" is always now */
	{"dec",			MONTH,		12},
	{"december",	MONTH,		12},
	{"dnt",			TZ,			6},				/* Dansk Normal Tid */
	{"dow",			RESERV,		DTK_DOW},		/* day of week */
	{"doy",			RESERV,		DTK_DOY},		/* day of year */
	{"dst",			DTZMOD,		6},
	{"east",		TZ,			NEG(60)},		/* East Australian Std Time */
	{"edt",			DTZ,		NEG(24)},		/* Eastern Daylight Time */
	{"eet",			TZ,			12},			/* East. Europe, USSR Zone 1 */
	{"eetdst",		DTZ,		18},			/* Eastern Europe */
	{EPOCH,			RESERV,		DTK_EPOCH},		/* "epoch" reserved for system epoch time */
#if USE_AUSTRALIAN_RULES
	{"est",			TZ,			60},			/* Australia Eastern Std Time */
#else
	{"est",			TZ,			NEG(30)},		/* Eastern Standard Time */
#endif
	{"feb",			MONTH,		2},
	{"february",	MONTH,		2},
	{"fri",			DOW,		5},
	{"friday",		DOW,		5},
	{"fst",			TZ,			6},				/* French Summer Time */
	{"fwt",			DTZ,		12},			/* French Winter Time  */
	{"gmt",			TZ,			0},				/* Greenwish Mean Time */
	{"gst",			TZ,			60},			/* Guam Std Time, USSR Zone 9 */
	{"hdt",			DTZ,		NEG(54)},		/* Hawaii/Alaska */
	{"hmt",			DTZ,		18},			/* Hellas ? ? */
	{"hst",			TZ,			NEG(60)},		/* Hawaii Std Time */
	{"idle",		TZ,			72},			/* Intl. Date Line,	East */
	{"idlw",		TZ,			NEG(72)},		/* Intl. Date Line,,	est */
	{LATE,			RESERV,		DTK_LATE},		/* "infinity" reserved for "late time" */
	{INVALID,		RESERV,		DTK_INVALID},	/* "invalid" reserved for invalid time */
	{"ist",			TZ,			12},			/* Israel */
	{"it",			TZ,			22},			/* Iran Time */
	{"jan",			MONTH,		1},
	{"january",		MONTH,		1},
	{"jst",			TZ,			54},			/* Japan Std Time,USSR Zone 8 */
	{"jt",			TZ,			45},			/* Java Time */
	{"jul",			MONTH,		7},
	{"july",		MONTH,		7},
	{"jun",			MONTH,		6},
	{"june",		MONTH,		6},
	{"kst",			TZ,			54},			/* Korea Standard Time */
	{"ligt",		TZ,			60},			/* From Melbourne, Australia */
	{"mar",			MONTH,		3},
	{"march",		MONTH,		3},
	{"may",			MONTH,		5},
	{"mdt",			DTZ,		NEG(36)},		/* Mountain Daylight Time */
	{"mest",		DTZ,		12},			/* Middle Europe Summer Time */
	{"met",			TZ,			6},				/* Middle Europe Time */
	{"metdst",		DTZ,		12},			/* Middle Europe Daylight Time */
	{"mewt",		TZ,			6},				/* Middle Europe Winter Time */
	{"mez",			TZ,			6},				/* Middle Europe Zone */
	{"mon",			DOW,		1},
	{"monday",		DOW,		1},
	{"mst",			TZ,			NEG(42)},		/* Mountain Standard Time */
	{"mt",			TZ,			51},			/* Moluccas Time */
	{"ndt",			DTZ,		NEG(15)},		/* Nfld. Daylight Time */
	{"nft",			TZ,			NEG(21)},		/* Newfoundland Standard Time */
	{"nor",			TZ,			6},				/* Norway Standard Time */
	{"nov",			MONTH,		11},
	{"november",	MONTH,		11},
	{NOW,			RESERV,		DTK_NOW},		/* current transaction time */
	{"nst",			TZ,			NEG(21)},		/* Nfld. Standard Time */
	{"nt",			TZ,			NEG(66)},		/* Nome Time */
	{"nzdt",		DTZ,		78},			/* New Zealand Daylight Time */
	{"nzst",		TZ,			72},			/* New Zealand Standard Time */
	{"nzt",			TZ,			72},			/* New Zealand Time */
	{"oct",			MONTH,		10},
	{"october",		MONTH,		10},
	{"on",			IGNORE,		0},				/* "on" (throwaway) */
	{"pdt",			DTZ,		NEG(42)},		/* Pacific Daylight Time */
	{"pm",			AMPM,		PM},
	{"pst",			TZ,			NEG(48)},		/* Pacific Standard Time */
	{"sadt",		DTZ,		63},			/* S. Australian Dayl. Time */
	{"sast",		TZ,			57},			/* South Australian Std Time */
	{"sat",			DOW,		6},
	{"saturday",	DOW,		6},
	{"sep",			MONTH,		9},
	{"sept",		MONTH,		9},
	{"september",	MONTH,		9},
	{"set",			TZ,			NEG(6)},		/* Seychelles Time ?? */
	{"sst",			DTZ,		12},			/* Swedish Summer Time */
	{"sun",			DOW,		0},
	{"sunday",		DOW,		0},
	{"swt",			TZ,			6},				/* Swedish Winter Time	*/
	{"thu",			DOW,		4},
	{"thur",		DOW,		4},
	{"thurs",		DOW,		4},
	{"thursday",	DOW,		4},
	{TODAY,			RESERV,		DTK_TODAY},		/* midnight */
	{TOMORROW,		RESERV,		DTK_TOMORROW},	/* tomorrow midnight */
	{"tue",			DOW,		2},
	{"tues",		DOW,		2},
	{"tuesday",		DOW,		2},
	{"undefined",	RESERV,		DTK_INVALID},	/* "undefined" pre-v6.1 invalid time */
	{"ut",			TZ,			0},
	{"utc",			TZ,			0},
	{"wadt",		DTZ,		48},			/* West Australian DST */
	{"wast",		TZ,			42},			/* West Australian Std Time */
	{"wat",			TZ,			NEG(6)},		/* West Africa Time */
	{"wdt",			DTZ,		54},			/* West Australian DST */
	{"wed",			DOW,		3},
	{"wednesday",	DOW,		3},
	{"weds",		DOW,		3},
	{"wet",			TZ,			0},				/* Western Europe */
	{"wetdst",		DTZ,		6},				/* Western Europe */
	{"wst",			TZ,			48},			/* West Australian Std Time */
	{"ydt",			DTZ,		NEG(48)},		/* Yukon Daylight Time */
	{YESTERDAY,		RESERV,		DTK_YESTERDAY},	/* yesterday midnight */
	{"yst",			TZ,			NEG(54)},		/* Yukon Standard Time */
	{"zp4",			TZ,			NEG(24)},		/* GMT +4  hours. */
	{"zp5",			TZ,			NEG(30)},		/* GMT +5  hours. */
	{"zp6",			TZ,			NEG(36)},		/* GMT +6  hours. */
	{"z",			RESERV,		DTK_ZULU},		/* 00:00:00 */
	{ZULU,			RESERV,		DTK_ZULU},		/* 00:00:00 */
};

static unsigned int szdatetktbl = sizeof datetktbl / sizeof datetktbl[0];

static datetkn deltatktbl[] = {
/*		text			token	lexval */
	{"@",			IGNORE,		0},				/* postgres relative time prefix */
	{DAGO,			AGO,		0},				/* "ago" indicates negative time offset */
	{"c",			UNITS,		DTK_CENTURY},	/* "century" relative time units */
	{"cent",		UNITS,		DTK_CENTURY},	/* "century" relative time units */
	{"centuries",	UNITS,		DTK_CENTURY},	/* "centuries" relative time units */
	{DCENTURY,		UNITS,		DTK_CENTURY},	/* "century" relative time units */
	{"d",			UNITS,		DTK_DAY},		/* "day" relative time units */
	{DDAY,			UNITS,		DTK_DAY},		/* "day" relative time units */
	{"days",		UNITS,		DTK_DAY},		/* "days" relative time units */
	{"dec",			UNITS,		DTK_DECADE},	/* "decade" relative time units */
	{"decs",		UNITS,		DTK_DECADE},	/* "decades" relative time units */
	{DDECADE,		UNITS,		DTK_DECADE},	/* "decade" relative time units */
	{"decades",		UNITS,		DTK_DECADE},	/* "decades" relative time units */
	{"h",			UNITS,		DTK_HOUR},		/* "hour" relative time units */
	{DHOUR,			UNITS,		DTK_HOUR},		/* "hour" relative time units */
	{"hours",		UNITS,		DTK_HOUR},		/* "hours" relative time units */
	{"hr",			UNITS,		DTK_HOUR},		/* "hour" relative time units */
	{"hrs",			UNITS,		DTK_HOUR},		/* "hours" relative time units */
	{INVALID,		RESERV,		DTK_INVALID},	/* "invalid" reserved for invalid time */
	{"m",			UNITS,		DTK_MINUTE},	/* "minute" relative time units */
	{"microsecon",	UNITS,		DTK_MILLISEC},	/* "microsecond" relative time units */
	{"mil",			UNITS,		DTK_MILLENIUM},	/* "millenium" relative time units */
	{"mils",		UNITS,		DTK_MILLENIUM},	/* "millenia" relative time units */
	{"millenia",	UNITS,		DTK_MILLENIUM},	/* "millenia" relative time units */
	{DMILLENIUM,	UNITS,		DTK_MILLENIUM},	/* "millenium" relative time units */
	{"millisecon",	UNITS,		DTK_MILLISEC},	/* "millisecond" relative time units */
	{"min",			UNITS,		DTK_MINUTE},	/* "minute" relative time units */
	{"mins",		UNITS,		DTK_MINUTE},	/* "minutes" relative time units */
	{"mins",		UNITS,		DTK_MINUTE},	/* "minutes" relative time units */
	{DMINUTE,		UNITS,		DTK_MINUTE},	/* "minute" relative time units */
	{"minutes",		UNITS,		DTK_MINUTE},	/* "minutes" relative time units */
	{"mon",			UNITS,		DTK_MONTH},		/* "months" relative time units */
	{"mons",		UNITS,		DTK_MONTH},		/* "months" relative time units */
	{DMONTH,		UNITS,		DTK_MONTH},		/* "month" relative time units */
	{"months",		UNITS,		DTK_MONTH},		/* "months" relative time units */
	{"ms",			UNITS,		DTK_MILLISEC},	/* "millisecond" relative time units */
	{"msec",		UNITS,		DTK_MILLISEC},	/* "millisecond" relative time units */
	{DMILLISEC,		UNITS,		DTK_MILLISEC},	/* "millisecond" relative time units */
	{"mseconds",	UNITS,		DTK_MILLISEC},	/* "milliseconds" relative time units */
	{"msecs",		UNITS,		DTK_MILLISEC},	/* "milliseconds" relative time units */
	{"qtr",			UNITS,		DTK_QUARTER},	/* "quarter" relative time units */
	{DQUARTER,		UNITS,		DTK_QUARTER},	/* "quarter" relative time units */
	{"reltime",		IGNORE,		0},				/* "reltime" for pre-v6.1 "Undefined Reltime" */
	{"s",			UNITS,		DTK_SECOND},	/* "second" relative time units */
	{"sec",			UNITS,		DTK_SECOND},	/* "second" relative time units */
	{DSECOND,		UNITS,		DTK_SECOND},	/* "second" relative time units */
	{"seconds",		UNITS,		DTK_SECOND},	/* "seconds" relative time units */
	{"secs",		UNITS,		DTK_SECOND},	/* "seconds" relative time units */
	{DTIMEZONE,		UNITS,		DTK_TZ},		/* "timezone" time offset */
	{"tz",			UNITS,		DTK_TZ},		/* "timezone" time offset */
	{"undefined",	RESERV,		DTK_INVALID},	/* "undefined" pre-v6.1 invalid time */
	{"us",			UNITS,		DTK_MICROSEC},	/* "microsecond" relative time units */
	{"usec",		UNITS,		DTK_MICROSEC},	/* "microsecond" relative time units */
	{DMICROSEC,		UNITS,		DTK_MICROSEC},	/* "microsecond" relative time units */
	{"useconds",	UNITS,		DTK_MICROSEC},	/* "microseconds" relative time units */
	{"usecs",		UNITS,		DTK_MICROSEC},	/* "microseconds" relative time units */
	{"w",			UNITS,		DTK_WEEK},		/* "week" relative time units */
	{DWEEK,			UNITS,		DTK_WEEK},		/* "week" relative time units */
	{"weeks",		UNITS,		DTK_WEEK},		/* "weeks" relative time units */
	{"y",			UNITS,		DTK_YEAR},		/* "year" relative time units */
	{DYEAR,			UNITS,		DTK_YEAR},		/* "year" relative time units */
	{"years",		UNITS,		DTK_YEAR},		/* "years" relative time units */
	{"yr",			UNITS,		DTK_YEAR},		/* "year" relative time units */
	{"yrs",			UNITS,		DTK_YEAR},		/* "years" relative time units */
};

static unsigned int szdeltatktbl = sizeof deltatktbl / sizeof deltatktbl[0];

#if USE_DATE_CACHE
datetkn    *datecache[MAXDATEFIELDS] = {NULL};

datetkn    *deltacache[MAXDATEFIELDS] = {NULL};

#endif


/*
 * Calendar time to Julian date conversions.
 * Julian date is commonly used in astronomical applications,
 *	since it is numerically accurate and computationally simple.
 * The algorithms here will accurately convert between Julian day
 *	and calendar date for all non-negative Julian days
 *	(i.e. from Nov 23, -4713 on).
 *
 * Ref: Explanatory Supplement to the Astronomical Almanac, 1992.
 *	University Science Books, 20 Edgehill Rd. Mill Valley CA 94941.
 *
 * Use the algorithm by Henry Fliegel, a former NASA/JPL colleague
 *	now at Aerospace Corp. (hi, Henry!)
 *
 * These routines will be used by other date/time packages - tgl 97/02/25
 */

/* Set the minimum year to one greater than the year of the first valid day
 *	to avoid having to check year and day both. - tgl 97/05/08
 */

#define JULIAN_MINYEAR (-4713)
#define JULIAN_MINMONTH (11)
#define JULIAN_MINDAY (23)

#define IS_VALID_JULIAN(y,m,d) ((y > JULIAN_MINYEAR) \
 || ((y == JULIAN_MINYEAR) && ((m > JULIAN_MINMONTH) \
  || ((m == JULIAN_MINMONTH) && (d >= JULIAN_MINDAY)))))

int
date2j(int y, int m, int d)
{
	int			m12 = (m - 14) / 12;

	return ((1461 * (y + 4800 + m12)) / 4 + (367 * (m - 2 - 12 * (m12))) / 12
			- (3 * ((y + 4900 + m12) / 100)) / 4 + d - 32075);
} /* date2j() */

void
j2date(int jd, int *year, int *month, int *day)
{
	int			j,
				y,
				m,
				d;

	int			i,
				l,
				n;

	l = jd + 68569;
	n = (4 * l) / 146097;
	l -= (146097 * n + 3) / 4;
	i = (4000 * (l + 1)) / 1461001;
	l += 31 - (1461 * i) / 4;
	j = (80 * l) / 2447;
	d = l - (2447 * j) / 80;
	l = j / 11;
	m = (j + 2) - (12 * l);
	y = 100 * (n - 49) + i + l;

	*year = y;
	*month = m;
	*day = d;
	return;
} /* j2date() */

static int
j2day(int date)
{
	int			day;

	day = (date + 1) % 7;

	return (day);
} /* j2day() */


/* datetime2tm()
 * Convert datetime data type to POSIX time structure.
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
datetime2tm(DateTime dt, int *tzp, struct tm * tm, double *fsec, char **tzn)
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

#ifdef DATEDEBUG
	printf("datetime2tm- date is %f (%f %f)\n", dt, date, time);
#endif

	j2date((int) date, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
	dt2time(time, &tm->tm_hour, &tm->tm_min, &sec);

#ifdef DATEDEBUG
	printf("datetime2tm- date is %d.%02d.%02d\n", tm->tm_year, tm->tm_mon, tm->tm_mday);
	printf("datetime2tm- time is %02d:%02d:%02.0f\n", tm->tm_hour, tm->tm_min, sec);
#endif

	*fsec = JROUND(sec);
	TMODULO(*fsec, tm->tm_sec, 1);

#ifdef DATEDEBUG
	printf("datetime2tm- time is %02d:%02d:%02d %.7f\n", tm->tm_hour, tm->tm_min, tm->tm_sec, *fsec);
#endif

	if (tzp != NULL)
	{
		if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
		{
			utime = (dt + (date0 - date2j(1970, 1, 1)) * 86400);
#if FALSE
			if (utime < -1)
				utime++;
#endif

#ifdef USE_POSIX_TIME
			tx = localtime(&utime);
#ifdef DATEDEBUG
#ifdef HAVE_INT_TIMEZONE
			printf("datetime2tm- (localtime) %d.%02d.%02d %02d:%02d:%02.0f %s %s dst=%d\n",
				   tx->tm_year, tx->tm_mon, tx->tm_mday, tx->tm_hour, tx->tm_min, sec,
				   tzname[0], tzname[1], tx->tm_isdst);
#else
			printf("datetime2tm- (localtime) %d.%02d.%02d %02d:%02d:%02.0f %s dst=%d\n",
				   tx->tm_year, tx->tm_mon, tx->tm_mday, tx->tm_hour, tx->tm_min, sec,
				   tx->tm_zone, tx->tm_isdst);
#endif
#else
#endif
			tm->tm_year = tx->tm_year + 1900;
			tm->tm_mon = tx->tm_mon + 1;
			tm->tm_mday = tx->tm_mday;
			tm->tm_hour = tx->tm_hour;
			tm->tm_min = tx->tm_min;
#if FALSE
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

#ifdef HAVE_INT_TIMEZONE
			*tzp = (tm->tm_isdst ? (timezone - 3600) : timezone);
			if (tzn != NULL)
				*tzn = tzname[(tm->tm_isdst > 0)];

#else							/* !HAVE_INT_TIMEZONE */
			tm->tm_gmtoff = tx->tm_gmtoff;
			tm->tm_zone = tx->tm_zone;

			*tzp = -(tm->tm_gmtoff);	/* tm_gmtoff is Sun/DEC-ism */
			if (tzn != NULL)
				*tzn = tm->tm_zone;
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

#ifdef DATEDEBUG
	printf("datetime2tm- date is %d.%02d.%02d\n", tm->tm_year, tm->tm_mon, tm->tm_mday);
	printf("datetime2tm- time is %02d:%02d:%02d %.7f\n", tm->tm_hour, tm->tm_min, tm->tm_sec, *fsec);
#endif

#ifdef DATEDEBUG
#ifdef USE_POSIX_TIME
#ifdef HAVE_INT_TIMEZONE
	printf("datetime2tm- timezone is %s; offset is %d (%d); daylight is %d\n",
		   tzname[tm->tm_isdst != 0], ((tzp != NULL) ? *tzp : 0), CTimeZone, CDayLight);
#endif
#endif
#endif

	return 0;
} /* datetime2tm() */


/* tm2datetime()
 * Convert a tm structure to a datetime data type.
 * Note that year is _not_ 1900-based, but is an explicit full value.
 * Also, month is one-based, _not_ zero-based.
 */
int
tm2datetime(struct tm * tm, double fsec, int *tzp, DateTime *result)
{

	double		date,
				time;

	/* Julian day routines are not correct for negative Julian days */
	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
		return (-1);

	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000, 1, 1);
	time = time2t(tm->tm_hour, tm->tm_min, (tm->tm_sec + fsec));
	*result = (date * 86400 + time);
#ifdef DATEDEBUG
	printf("tm2datetime- date is %f (%f %f %d)\n", *result, date, time, (((tm->tm_hour * 60) + tm->tm_min) * 60 + tm->tm_sec));
	printf("tm2datetime- time is %f %02d:%02d:%02d %f\n", time, tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);
#endif
	if (tzp != NULL)
		*result = dt2local(*result, -(*tzp));

	return 0;
} /* tm2datetime() */


/* timespan2tm()
 * Convert a timespan data type to a tm structure.
 */
int
timespan2tm(TimeSpan span, struct tm * tm, float8 *fsec)
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
	TMODULO(time, tm->tm_sec, 1);
	*fsec = time;

#ifdef DATEDEBUG
	printf("timespan2tm- %d %f = %04d-%02d-%02d %02d:%02d:%02d %.2f\n", span.month, span.time,
		   tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, *fsec);
#endif

	return 0;
} /* timespan2tm() */

int
tm2timespan(struct tm * tm, double fsec, TimeSpan *span)
{
	span->month = ((tm->tm_year * 12) + tm->tm_mon);
	span->time = ((((((tm->tm_mday * 24) + tm->tm_hour) * 60) + tm->tm_min) * 60) + tm->tm_sec);
	span->time = JROUND(span->time + fsec);

#ifdef DATEDEBUG
	printf("tm2timespan- %d %f = %04d-%02d-%02d %02d:%02d:%02d %.2f\n", span->month, span->time,
		   tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);
#endif

	return 0;
} /* tm2timespan() */


static DateTime
dt2local(DateTime dt, int tz)
{
	dt -= tz;
	dt = JROUND(dt);
	return (dt);
} /* dt2local() */

double
time2t(const int hour, const int min, const double sec)
{
	return ((((hour * 60) + min) * 60) + sec);
} /* time2t() */

static void
dt2time(DateTime jd, int *hour, int *min, double *sec)
{
	double		time;

	time = jd;

	*hour = (time / 3600);
	time -= ((*hour) * 3600);
	*min = (time / 60);
	time -= ((*min) * 60);
	*sec = JROUND(time);

	return;
} /* dt2time() */


/*
 * parse and convert date in timestr (the normal interface)
 *
 * Returns the number of seconds since epoch (J2000)
 */

/* ParseDateTime()
 * Break string into tokens based on a date/time context.
 */
int
ParseDateTime(char *timestr, char *lowstr,
			  char *field[], int ftype[], int maxfields, int *numfields)
{
	int			nf = 0;
	char	   *cp = timestr;
	char	   *lp = lowstr;

#ifdef DATEDEBUG
	printf("ParseDateTime- input string is %s\n", timestr);
#endif
	/* outer loop through fields */
	while (*cp != '\0')
	{
		field[nf] = lp;

		/* leading digit? then date or time */
		if (isdigit(*cp) || (*cp == '.'))
		{
			*lp++ = *cp++;
			while (isdigit(*cp))
				*lp++ = *cp++;
			/* time field? */
			if (*cp == ':')
			{
				ftype[nf] = DTK_TIME;
				while (isdigit(*cp) || (*cp == ':') || (*cp == '.'))
					*lp++ = *cp++;

			}
			/* date field? allow embedded text month */
			else if ((*cp == '-') || (*cp == '/') || (*cp == '.'))
			{
				ftype[nf] = DTK_DATE;
				while (isalnum(*cp) || (*cp == '-') || (*cp == '/') || (*cp == '.'))
					*lp++ = tolower(*cp++);

			}
			/* otherwise, number only and will determine year, month, or day later */
			else
			{
				ftype[nf] = DTK_NUMBER;
			}

		}
		/* text? then date string, month, day of week, special, or timezone */
		else if (isalpha(*cp))
		{
			ftype[nf] = DTK_STRING;
			*lp++ = tolower(*cp++);
			while (isalpha(*cp))
				*lp++ = tolower(*cp++);

			/* full date string with leading text month? */
			if ((*cp == '-') || (*cp == '/') || (*cp == '.'))
			{
				ftype[nf] = DTK_DATE;
				while (isdigit(*cp) || (*cp == '-') || (*cp == '/') || (*cp == '.'))
					*lp++ = tolower(*cp++);
			}

			/* skip leading spaces */
		}
		else if (isspace(*cp))
		{
			cp++;
			continue;

			/* sign? then special or numeric timezone */
		}
		else if ((*cp == '+') || (*cp == '-'))
		{
			*lp++ = *cp++;
			/* soak up leading whitespace */
			while (isspace(*cp))
				cp++;
			/* numeric timezone? */
			if (isdigit(*cp))
			{
				ftype[nf] = DTK_TZ;
				*lp++ = *cp++;
				while (isdigit(*cp) || (*cp == ':'))
					*lp++ = *cp++;

				/* special? */
			}
			else if (isalpha(*cp))
			{
				ftype[nf] = DTK_SPECIAL;
				*lp++ = tolower(*cp++);
				while (isalpha(*cp))
					*lp++ = tolower(*cp++);

				/* otherwise something wrong... */
			}
			else
			{
				return -1;
			}

			/* ignore punctuation but use as delimiter */
		}
		else if (ispunct(*cp))
		{
			cp++;
			continue;

		}
		else
		{
			return -1;
		}

		/* force in a delimiter */
		*lp++ = '\0';
		nf++;
		if (nf > MAXDATEFIELDS)
		{
			return -1;
		}
#ifdef DATEDEBUG
		printf("ParseDateTime- set field[%d] to %s type %d\n", (nf - 1), field[nf - 1], ftype[nf - 1]);
#endif
	}

	*numfields = nf;

	return 0;
} /* ParseDateTime() */


/* DecodeDateTime()
 * Interpret previously parsed fields for general date and time.
 * Return 0 if full date, 1 if only time, and -1 if problems.
 *		External format(s):
 *				"<weekday> <month>-<day>-<year> <hour>:<minute>:<second>"
 *				"Fri Feb-7-1997 15:23:27"
 *				"Feb-7-1997 15:23:27"
 *				"2-7-1997 15:23:27"
 *				"1997-2-7 15:23:27"
 *				"1997.038 15:23:27"		(day of year 1-366)
 *		Also supports input in compact time:
 *				"970207 152327"
 *				"97038 152327"
 *
 * Use the system-provided functions to get the current time zone
 *	if not specified in the input string.
 * If the date is outside the time_t system-supported time range,
 *	then assume GMT time zone. - tgl 97/05/27
 */
int
DecodeDateTime(char *field[], int ftype[], int nf,
			   int *dtype, struct tm * tm, double *fsec, int *tzp)
{
	int			fmask = 0,
				tmask,
				type;
	int			i;
	int			flen,
				val;
	int			mer = HR24;
	int			bc = FALSE;

	*dtype = DTK_DATE;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	*fsec = 0;
	tm->tm_isdst = -1;			/* don't know daylight savings time status
								 * apriori */
	if (tzp != NULL)
		*tzp = 0;

	for (i = 0; i < nf; i++)
	{
#ifdef DATEDEBUG
		printf("DecodeDateTime- field[%d] is %s (type %d)\n", i, field[i], ftype[i]);
#endif
		switch (ftype[i])
		{
			case DTK_DATE:
				if (DecodeDate(field[i], fmask, &tmask, tm) != 0)
					return -1;
				break;

			case DTK_TIME:
				if (DecodeTime(field[i], fmask, &tmask, tm, fsec) != 0)
					return -1;

				/*
				 * check upper limit on hours; other limits checked in
				 * DecodeTime()
				 */
				if (tm->tm_hour > 23)
					return -1;
				break;

			case DTK_TZ:
				if (tzp == NULL)
					return -1;
				if (DecodeTimezone(field[i], tzp) != 0)
					return -1;
				tmask = DTK_M(TZ);
				break;

			case DTK_NUMBER:
				flen = strlen(field[i]);

				if (flen > 4)
				{
					if (DecodeNumberField(flen, field[i], fmask, &tmask, tm, fsec) != 0)
						return -1;

				}
				else
				{
					if (DecodeNumber(flen, field[i], fmask, &tmask, tm, fsec) != 0)
						return -1;
				}
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				type = DecodeSpecial(i, field[i], &val);
#ifdef DATEDEBUG
				printf("DecodeDateTime- special field[%d] %s type=%d value=%d\n", i, field[i], type, val);
#endif
				if (type == IGNORE)
					continue;

				tmask = DTK_M(type);
				switch (type)
				{
					case RESERV:
#ifdef DATEDEBUG
						printf("DecodeDateTime- RESERV field %s value is %d\n", field[i], val);
#endif
						switch (val)
						{
							case DTK_NOW:
								tmask = (DTK_DATE_M | DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_DATE;
								GetCurrentTime(tm);
								if (tzp != NULL)
									*tzp = CTimeZone;
								break;

							case DTK_YESTERDAY:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentTime(tm);
								j2date((date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - 1),
								&tm->tm_year, &tm->tm_mon, &tm->tm_mday);
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								break;

							case DTK_TODAY:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentTime(tm);
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								break;

							case DTK_TOMORROW:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentTime(tm);
								j2date((date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) + 1),
								&tm->tm_year, &tm->tm_mon, &tm->tm_mday);
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								break;

							case DTK_ZULU:
								tmask = (DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_DATE;
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								if (tzp != NULL)
									*tzp = 0;
								break;

							default:
								*dtype = val;
						}

						break;

					case MONTH:
#ifdef DATEDEBUG
						printf("DecodeDateTime- month field %s value is %d\n", field[i], val);
#endif
						tm->tm_mon = val;
						break;

						/*
						 * daylight savings time modifier (solves "MET
						 * DST" syntax)
						 */
					case DTZMOD:
						tmask |= DTK_M(DTZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return -1;
						*tzp += val * 60;
						break;

					case DTZ:

						/*
						 * set mask for TZ here _or_ check for DTZ later
						 * when getting default timezone
						 */
						tmask |= DTK_M(TZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return -1;
						*tzp = val * 60;
						break;

					case TZ:
						tm->tm_isdst = 0;
						if (tzp == NULL)
							return -1;
						*tzp = val * 60;
						break;

					case IGNORE:
						break;

					case AMPM:
						mer = val;
						break;

					case ADBC:
						bc = (val == BC);
						break;

					case DOW:
						tm->tm_wday = val;
						break;

					default:
						return -1;
				}
				break;

			default:
				return -1;
		}

#ifdef DATEDEBUG
		printf("DecodeDateTime- field[%d] %s (%08x/%08x) value is %d\n",
			   i, field[i], fmask, tmask, val);
#endif

		if (tmask & fmask)
			return -1;
		fmask |= tmask;
	}

	/* there is no year zero in AD/BC notation; i.e. "1 BC" == year 0 */
	if (bc)
		tm->tm_year = -(tm->tm_year - 1);

	if ((mer != HR24) && (tm->tm_hour > 12))
		return -1;
	if ((mer == AM) && (tm->tm_hour == 12))
		tm->tm_hour = 0;
	else if ((mer == PM) && (tm->tm_hour != 12))
		tm->tm_hour += 12;

#ifdef DATEDEBUG
	printf("DecodeDateTime- mask %08x (%08x)", fmask, DTK_DATE_M);
	printf(" set y%04d m%02d d%02d", tm->tm_year, tm->tm_mon, tm->tm_mday);
	printf(" %02d:%02d:%02d\n", tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif

	if ((*dtype == DTK_DATE) && ((fmask & DTK_DATE_M) != DTK_DATE_M))
		return (((fmask & DTK_TIME_M) == DTK_TIME_M) ? 1 : -1);

	/* timezone not specified? then find local timezone if possible */
	if ((*dtype == DTK_DATE) && ((fmask & DTK_DATE_M) == DTK_DATE_M)
		&& (tzp != NULL) && (!(fmask & DTK_M(TZ))))
	{

		/*
		 * daylight savings time modifier but no standard timezone? then
		 * error
		 */
		if (fmask & DTK_M(DTZMOD))
			return -1;

		if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
		{
#ifdef USE_POSIX_TIME
			tm->tm_year -= 1900;
			tm->tm_mon -= 1;
			tm->tm_isdst = -1;
			mktime(tm);
			tm->tm_year += 1900;
			tm->tm_mon += 1;

#ifdef HAVE_INT_TIMEZONE
			*tzp = ((tm->tm_isdst > 0) ? (timezone - 3600) : timezone);

#else							/* !HAVE_INT_TIMEZONE */
			*tzp = -(tm->tm_gmtoff);	/* tm_gmtoff is Sun/DEC-ism */
#endif

#else							/* !USE_POSIX_TIME */
			*tzp = CTimeZone;
#endif
		}
		else
		{
			tm->tm_isdst = 0;
			*tzp = 0;
		}
	}

	return 0;
} /* DecodeDateTime() */


/* DecodeTimeOnly()
 * Interpret parsed string as time fields only.
 */
int
DecodeTimeOnly(char *field[], int ftype[], int nf, int *dtype, struct tm * tm, double *fsec)
{
	int			fmask,
				tmask,
				type;
	int			i;
	int			flen,
				val;
	int			mer = HR24;

	*dtype = DTK_TIME;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	tm->tm_isdst = -1;			/* don't know daylight savings time status apriori */
	*fsec = 0;

	fmask = DTK_DATE_M;

	for (i = 0; i < nf; i++)
	{
#ifdef DATEDEBUG
		printf("DecodeTimeOnly- field[%d] is %s (type %d)\n", i, field[i], ftype[i]);
#endif
		switch (ftype[i])
		{
			case DTK_TIME:
				if (DecodeTime(field[i], fmask, &tmask, tm, fsec) != 0)
					return -1;
				break;

			case DTK_NUMBER:
				flen = strlen(field[i]);

				if (DecodeNumberField(flen, field[i], fmask, &tmask, tm, fsec) != 0)
					return -1;
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				type = DecodeSpecial(i, field[i], &val);
#ifdef DATEDEBUG
				printf("DecodeTimeOnly- special field[%d] %s type=%d value=%d\n", i, field[i], type, val);
#endif
				if (type == IGNORE)
					continue;

				tmask = DTK_M(type);
				switch (type)
				{
					case RESERV:
#ifdef DATEDEBUG
						printf("DecodeTimeOnly- RESERV field %s value is %d\n", field[i], val);
#endif
						switch (val)
						{
							case DTK_NOW:
								tmask = DTK_TIME_M;
								*dtype = DTK_TIME;
								GetCurrentTime(tm);
								break;

							case DTK_ZULU:
								tmask = (DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_TIME;
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								tm->tm_isdst = 0;
								break;

							default:
								return -1;
						}

						break;

					case IGNORE:
						break;

					case AMPM:
						mer = val;
						break;

					default:
						return -1;
				}
				break;

			default:
				return -1;
		}

		if (tmask & fmask)
			return -1;
		fmask |= tmask;

#ifdef DATEDEBUG
		printf("DecodeTimeOnly- field[%d] %s value is %d\n", i, field[i], val);
#endif
	}

#ifdef DATEDEBUG
	printf("DecodeTimeOnly- mask %08x (%08x)", fmask, DTK_TIME_M);
	printf(" %02d:%02d:%02d (%f)\n", tm->tm_hour, tm->tm_min, tm->tm_sec, *fsec);
#endif

	if ((mer != HR24) && (tm->tm_hour > 12))
		return -1;
	if ((mer == AM) && (tm->tm_hour == 12))
		tm->tm_hour = 0;
	else if ((mer == PM) && (tm->tm_hour != 12))
		tm->tm_hour += 12;

	if ((fmask & DTK_TIME_M) != DTK_TIME_M)
		return -1;

	return 0;
} /* DecodeTimeOnly() */


/* DecodeDate()
 * Decode date string which includes delimiters.
 * Insist on a complete set of fields.
 */
static int
DecodeDate(char *str, int fmask, int *tmask, struct tm * tm)
{
	double		fsec;

	int			nf = 0;
	int			i,
				len;
	int			type,
				val,
				dmask = 0;
	char	   *field[MAXDATEFIELDS];

	/* parse this string... */
	while ((*str != '\0') && (nf < MAXDATEFIELDS))
	{
		/* skip field separators */
		while (!isalnum(*str))
			str++;

		field[nf] = str;
		if (isdigit(*str))
		{
			while (isdigit(*str))
				str++;
		}
		else if (isalpha(*str))
		{
			while (isalpha(*str))
				str++;
		}

		if (*str != '\0')
			*str++ = '\0';
		nf++;
	}

	/* don't allow too many fields */
	if (nf > 3)
		return -1;

	*tmask = 0;

	/* look first for text fields, since that will be unambiguous month */
	for (i = 0; i < nf; i++)
	{
		if (isalpha(*field[i]))
		{
			type = DecodeSpecial(i, field[i], &val);
			if (type == IGNORE)
				continue;

			dmask = DTK_M(type);
			switch (type)
			{
				case MONTH:
#ifdef DATEDEBUG
					printf("DecodeDate- month field %s value is %d\n", field[i], val);
#endif
					tm->tm_mon = val;
					break;

				default:
#ifdef DATEDEBUG
					printf("DecodeDate- illegal field %s value is %d\n", field[i], val);
#endif
					return -1;
			}
			if (fmask & dmask)
				return -1;

			fmask |= dmask;
			*tmask |= dmask;

			/* mark this field as being completed */
			field[i] = NULL;
		}
	}

	/* now pick up remaining numeric fields */
	for (i = 0; i < nf; i++)
	{
		if (field[i] == NULL)
			continue;

		if ((len = strlen(field[i])) <= 0)
			return -1;

		if (DecodeNumber(len, field[i], fmask, &dmask, tm, &fsec) != 0)
			return -1;

		if (fmask & dmask)
			return -1;

		fmask |= dmask;
		*tmask |= dmask;
	}

	return 0;
} /* DecodeDate() */


/* DecodeTime()
 * Decode time string which includes delimiters.
 * Only check the lower limit on hours, since this same code
 *	can be used to represent time spans.
 */
static int
DecodeTime(char *str, int fmask, int *tmask, struct tm * tm, double *fsec)
{
	char	   *cp;

	*tmask = DTK_TIME_M;

	tm->tm_hour = strtol(str, &cp, 10);
	if (*cp != ':')
		return -1;
	str = cp + 1;
	tm->tm_min = strtol(str, &cp, 10);
	if (*cp == '\0')
	{
		tm->tm_sec = 0;
		*fsec = 0;

	}
	else if (*cp != ':')
	{
		return -1;

	}
	else
	{
		str = cp + 1;
		tm->tm_sec = strtol(str, &cp, 10);
		if (*cp == '\0')
		{
			*fsec = 0;
		}
		else if (*cp == '.')
		{
			str = cp;
			*fsec = strtod(str, &cp);
			if (cp == str)
				return -1;
		}
		else
		{
			return -1;
		}
	}

	/* do a sanity check */
	if ((tm->tm_hour < 0)
		|| (tm->tm_min < 0) || (tm->tm_min > 59)
		|| (tm->tm_sec < 0) || (tm->tm_sec > 59))
		return -1;

	return 0;
} /* DecodeTime() */


/* DecodeNumber()
 * Interpret numeric field as a date value in context.
 */
static int
DecodeNumber(int flen, char *str, int fmask, int *tmask, struct tm * tm, double *fsec)
{
	int			val;
	char	   *cp;

	*tmask = 0;

	val = strtol(str, &cp, 10);
	if (cp == str)
		return -1;
	if (*cp == '.')
	{
		*fsec = strtod(cp, &cp);
		if (*cp != '\0')
			return -1;
	}

#ifdef DATEDEBUG
	printf("DecodeNumber- %s is %d fmask=%08x tmask=%08x\n", str, val, fmask, *tmask);
#endif

	/* enough digits to be unequivocal year? */
	if (flen == 4)
	{
#ifdef DATEDEBUG
		printf("DecodeNumber- match %d (%s) as year\n", val, str);
#endif
		*tmask = DTK_M(YEAR);

		/* already have a year? then see if we can substitute... */
		if (fmask & DTK_M(YEAR))
		{
			if ((!(fmask & DTK_M(DAY)))
				&& ((tm->tm_year >= 1) && (tm->tm_year <= 31)))
			{
#ifdef DATEDEBUG
				printf("DecodeNumber- misidentified year previously; swap with day %d\n", tm->tm_mday);
#endif
				tm->tm_mday = tm->tm_year;
				*tmask = DTK_M(DAY);
			}
		}

		tm->tm_year = val;

		/* special case day of year? */
	}
	else if ((flen == 3) && (fmask & DTK_M(YEAR))
			 && ((val >= 1) && (val <= 366)))
	{
		*tmask = (DTK_M(DOY) | DTK_M(MONTH) | DTK_M(DAY));
		tm->tm_yday = val;
		j2date((date2j(tm->tm_year, 1, 1) + tm->tm_yday - 1),
			   &tm->tm_year, &tm->tm_mon, &tm->tm_mday);

		/* already have year? then could be month */
	}
	else if ((fmask & DTK_M(YEAR)) && (!(fmask & DTK_M(MONTH)))
			 && ((val >= 1) && (val <= 12)))
	{
#ifdef DATEDEBUG
		printf("DecodeNumber- match %d (%s) as month\n", val, str);
#endif
		*tmask = DTK_M(MONTH);
		tm->tm_mon = val;

		/* no year and EuroDates enabled? then could be day */
	}
	else if ((EuroDates || (fmask & DTK_M(MONTH)))
			 && (!(fmask & DTK_M(YEAR)) && !(fmask & DTK_M(DAY)))
			 && ((val >= 1) && (val <= 31)))
	{
#ifdef DATEDEBUG
		printf("DecodeNumber- match %d (%s) as day\n", val, str);
#endif
		*tmask = DTK_M(DAY);
		tm->tm_mday = val;

	}
	else if ((!(fmask & DTK_M(MONTH)))
			 && ((val >= 1) && (val <= 12)))
	{
#ifdef DATEDEBUG
		printf("DecodeNumber- (2) match %d (%s) as month\n", val, str);
#endif
		*tmask = DTK_M(MONTH);
		tm->tm_mon = val;

	}
	else if ((!(fmask & DTK_M(DAY)))
			 && ((val >= 1) && (val <= 31)))
	{
#ifdef DATEDEBUG
		printf("DecodeNumber- (2) match %d (%s) as day\n", val, str);
#endif
		*tmask = DTK_M(DAY);
		tm->tm_mday = val;

	}
	else if (!(fmask & DTK_M(YEAR)))
	{
#ifdef DATEDEBUG
		printf("DecodeNumber- (2) match %d (%s) as year\n", val, str);
#endif
		*tmask = DTK_M(YEAR);
		tm->tm_year = val;
		if (tm->tm_year < 70)
		{
			tm->tm_year += 2000;
		}
		else if (tm->tm_year < 100)
		{
			tm->tm_year += 1900;
		}

	}
	else
	{
		return -1;
	}

	return 0;
} /* DecodeNumber() */


/* DecodeNumberField()
 * Interpret numeric string as a concatenated date field.
 */
static int
DecodeNumberField(int len, char *str, int fmask, int *tmask, struct tm * tm, double *fsec)
{
	char	   *cp;

	/* yyyymmdd? */
	if (len == 8)
	{
#ifdef DATEDEBUG
		printf("DecodeNumberField- %s is 8 character date fmask=%08x tmask=%08x\n", str, fmask, *tmask);
#endif

		*tmask = DTK_DATE_M;

		tm->tm_mday = atoi(str + 6);
		*(str + 6) = '\0';
		tm->tm_mon = atoi(str + 4);
		*(str + 4) = '\0';
		tm->tm_year = atoi(str + 0);

		/* yymmdd or hhmmss? */
	}
	else if (len == 6)
	{
#ifdef DATEDEBUG
		printf("DecodeNumberField- %s is 6 characters fmask=%08x tmask=%08x\n", str, fmask, *tmask);
#endif
		if (fmask & DTK_DATE_M)
		{
#ifdef DATEDEBUG
			printf("DecodeNumberField- %s is time field fmask=%08x tmask=%08x\n", str, fmask, *tmask);
#endif
			*tmask = DTK_TIME_M;
			tm->tm_sec = atoi(str + 4);
			*(str + 4) = '\0';
			tm->tm_min = atoi(str + 2);
			*(str + 2) = '\0';
			tm->tm_hour = atoi(str + 0);

		}
		else
		{
#ifdef DATEDEBUG
			printf("DecodeNumberField- %s is date field fmask=%08x tmask=%08x\n", str, fmask, *tmask);
#endif
			*tmask = DTK_DATE_M;
			tm->tm_mday = atoi(str + 4);
			*(str + 4) = '\0';
			tm->tm_mon = atoi(str + 2);
			*(str + 2) = '\0';
			tm->tm_year = atoi(str + 0);
		}

	}
	else if (strchr(str, '.') != NULL)
	{
#ifdef DATEDEBUG
		printf("DecodeNumberField- %s is time field fmask=%08x tmask=%08x\n", str, fmask, *tmask);
#endif
		*tmask = DTK_TIME_M;
		tm->tm_sec = strtod((str + 4), &cp);
		if (cp == (str + 4))
			return -1;
		if (*cp == '.')
		{
			*fsec = strtod(cp, NULL);
		}
		*(str + 4) = '\0';
		tm->tm_min = strtod((str + 2), &cp);
		*(str + 2) = '\0';
		tm->tm_hour = strtod((str + 0), &cp);

	}
	else
	{
		return -1;
	}

	return 0;
} /* DecodeNumberField() */


/* DecodeTimezone()
 * Interpret string as a numeric timezone.
 */
static int
DecodeTimezone(char *str, int *tzp)
{
	int			tz;
	int			hr,
				min;
	char	   *cp;
	int			len;

	/* assume leading character is "+" or "-" */
	hr = strtol((str + 1), &cp, 10);

	/* explicit delimiter? */
	if (*cp == ':')
	{
		min = strtol((cp + 1), &cp, 10);

		/* otherwise, might have run things together... */
	}
	else if ((*cp == '\0') && ((len = strlen(str)) > 3))
	{
		min = strtol((str + len - 2), &cp, 10);
		*(str + len - 2) = '\0';
		hr = strtol((str + 1), &cp, 10);

	}
	else
	{
		min = 0;
	}

	tz = (hr * 60 + min) * 60;
	if (*str == '-')
		tz = -tz;

	*tzp = -tz;
	return (*cp != '\0');
} /* DecodeTimezone() */


/* DecodeSpecial()
 * Decode text string using lookup table.
 * Implement a cache lookup since it is likely that dates
 *	will be related in format.
 */
static int
DecodeSpecial(int field, char *lowtoken, int *val)
{
	int			type;
	datetkn    *tp;

#if USE_DATE_CACHE
	if ((datecache[field] != NULL)
		&& (strncmp(lowtoken, datecache[field]->token, TOKMAXLEN) == 0))
	{
		tp = datecache[field];
	}
	else
	{
#endif
		tp = datebsearch(lowtoken, datetktbl, szdatetktbl);
#if USE_DATE_CACHE
	}
	datecache[field] = tp;
#endif
	if (tp == NULL)
	{
		type = IGNORE;
		*val = 0;
	}
	else
	{
		type = tp->type;
		switch (type)
		{
			case TZ:
			case DTZ:
			case DTZMOD:
				*val = FROMVAL(tp);
				break;

			default:
				*val = tp->value;
				break;
		}
	}

	return (type);
} /* DecodeSpecial() */


/* DecodeDateDelta()
 * Interpret previously parsed fields for general time interval.
 * Return 0 if decoded and -1 if problems.
 *
 * Allow "date" field DTK_DATE since this could be just
 *  an unsigned floating point number. - thomas 1997-11-16
 *
 * If code is changed to read fields from first to last,
 *	then use READ_FORWARD-bracketed code to allow sign
 *	to persist to subsequent unsigned fields.
 */
int
DecodeDateDelta(char *field[], int ftype[], int nf, int *dtype, struct tm * tm, double *fsec)
{
	int			is_before = FALSE;

#if READ_FORWARD
	int			is_neg = FALSE;
#endif

	char	   *cp;
	int			fmask = 0,
				tmask,
				type;
	int			i,
				ii;
	int			flen,
				val;
	double		fval;
	double		sec;

	*dtype = DTK_DELTA;

	type = DTK_SECOND;
	tm->tm_year = 0;
	tm->tm_mon = 0;
	tm->tm_mday = 0;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	*fsec = 0;

	/* read through list forwards to pick up initial time fields, if any */
	for (ii = 0; ii < nf; ii++)
	{
#ifdef DATEDEBUG
		printf("DecodeDateDelta- field[%d] is %s (type %d)\n", ii, field[ii], ftype[ii]);
#endif
		if (ftype[ii] == DTK_TIME)
		{
			if (DecodeTime(field[ii], fmask, &tmask, tm, fsec) != 0)
				return -1;
			fmask |= tmask;

		}
		else
		{
			break;
		}
	}

	/*
	 * read through remaining list backwards to pick up units before values
	 */
	for (i = nf - 1; i >= ii; i--)
	{
#ifdef DATEDEBUG
		printf("DecodeDateDelta- field[%d] is %s (type %d)\n", i, field[i], ftype[i]);
#endif
		switch (ftype[i])
		{
			case DTK_TIME:
				/* already read in forward-scan above so return error */
#if FALSE
				if (DecodeTime(field[i], fmask, &tmask, tm, fsec) != 0)
					return -1;
#endif
				return -1;
				break;

			case DTK_TZ:		/* timezone is a token with a leading sign
								 * character */
#if READ_FORWARD
				is_neg = (*field[i] == '-');
#endif

			case DTK_DATE:
			case DTK_NUMBER:
				val = strtol(field[i], &cp, 10);
#if READ_FORWARD
				if (is_neg && (val > 0))
					val = -val;
#endif
				if (*cp == '.')
				{
					fval = strtod(cp, &cp);
					if (*cp != '\0')
						return -1;

					if (val < 0)
						fval = -(fval);
#if FALSE
					*fsec = strtod(cp, NULL);
					if (val < 0)
						*fsec = -(*fsec);
#endif
				}
				else if (*cp == '\0')
					fval = 0;
				else
					return -1;

				flen = strlen(field[i]);
				tmask = 0;		/* DTK_M(type); */

				switch (type)
				{
					case DTK_MICROSEC:
						*fsec += ((val + fval) * 1e-6);
						break;

					case DTK_MILLISEC:
						*fsec += ((val +fval) * 1e-3);
						break;

					case DTK_SECOND:
						tm->tm_sec += val;
						*fsec += fval;
						tmask = DTK_M(SECOND);
						break;

					case DTK_MINUTE:
						tm->tm_min += val;
						if (fval != 0)
							tm->tm_sec += (fval * 60);
						tmask = DTK_M(MINUTE);
						break;

					case DTK_HOUR:
						tm->tm_hour += val;
						if (fval != 0)
							tm->tm_sec += (fval * 3600);
						tmask = DTK_M(HOUR);
						break;

					case DTK_DAY:
						tm->tm_mday += val;
						if (fval != 0)
							tm->tm_sec += (fval * 86400);
						tmask = ((fmask & DTK_M(DAY)) ? 0 : DTK_M(DAY));
						break;

					case DTK_WEEK:
						tm->tm_mday += val * 7;
						if (fval != 0)
							tm->tm_sec += (fval * (7*86400));
						tmask = ((fmask & DTK_M(DAY)) ? 0 : DTK_M(DAY));
						break;

					case DTK_MONTH:
						tm->tm_mon += val;
						if (fval != 0)
							tm->tm_sec += (fval * (30*86400));
						tmask = DTK_M(MONTH);
						break;

					case DTK_YEAR:
						tm->tm_year += val;
						if (fval != 0)
							tm->tm_mon += (fval * 12);
						tmask = ((fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR));
						break;

					case DTK_DECADE:
						tm->tm_year += val * 10;
						if (fval != 0)
							tm->tm_mon += (fval * 120);
						tmask = ((fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR));
						break;

					case DTK_CENTURY:
						tm->tm_year += val * 100;
						if (fval != 0)
							tm->tm_mon += (fval * 1200);
						tmask = ((fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR));
						break;

					case DTK_MILLENIUM:
						tm->tm_year += val * 1000;
						if (fval != 0)
							tm->tm_mon += (fval * 12000);
						tmask = ((fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR));
						break;

					default:
						return -1;
				}
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				type = DecodeUnits(i, field[i], &val);
#ifdef DATEDEBUG
				printf("DecodeDateDelta- special field[%d] %s type=%d value=%d\n", i, field[i], type, val);
#endif
				if (type == IGNORE)
					continue;

				tmask = 0;		/* DTK_M(type); */
				switch (type)
				{
					case UNITS:
#ifdef DATEDEBUG
						printf("DecodeDateDelta- UNITS field %s value is %d\n", field[i], val);
#endif
						type = val;
						break;

					case AGO:
						is_before = TRUE;
						type = val;
						break;

					case RESERV:
						tmask = (DTK_DATE_M || DTK_TIME_M);
						*dtype = val;
						break;

					default:
						return -1;
				}
				break;

			default:
				return -1;
		}

#ifdef DATEDEBUG
		printf("DecodeDateDelta- (%08x/%08x) field[%d] %s value is %d\n",
			   fmask, tmask, i, field[i], val);
#endif

		if (tmask & fmask)
			return -1;
		fmask |= tmask;
	}

	if (*fsec != 0)
	{
		TMODULO(*fsec, sec, 1);
		tm->tm_sec += sec;
	}

	if (is_before)
	{
		*fsec = -(*fsec);
		tm->tm_sec = -(tm->tm_sec);
		tm->tm_min = -(tm->tm_min);
		tm->tm_hour = -(tm->tm_hour);
		tm->tm_mday = -(tm->tm_mday);
		tm->tm_mon = -(tm->tm_mon);
		tm->tm_year = -(tm->tm_year);
	}

#ifdef DATEDEBUG
	printf("DecodeDateDelta- mask %08x (%08x)", fmask, DTK_DATE_M);
	printf(" set y%04d m%02d d%02d", tm->tm_year, tm->tm_mon, tm->tm_mday);
	printf(" %02d:%02d:%02d\n", tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif

	/* ensure that at least one time field has been found */
	return ((fmask != 0) ? 0 : -1);
} /* DecodeDateDelta() */


/* DecodeUnits()
 * Decode text string using lookup table.
 * This routine supports time interval decoding.
 */
static int
DecodeUnits(int field, char *lowtoken, int *val)
{
	int			type;
	datetkn    *tp;

#if USE_DATE_CACHE
	if ((deltacache[field] != NULL)
		&& (strncmp(lowtoken, deltacache[field]->token, TOKMAXLEN) == 0))
	{
		tp = deltacache[field];
	}
	else
	{
#endif
		tp = datebsearch(lowtoken, deltatktbl, szdeltatktbl);
#if USE_DATE_CACHE
	}
	deltacache[field] = tp;
#endif
	if (tp == NULL)
	{
		type = IGNORE;
		*val = 0;
	}
	else
	{
		type = tp->type;
		if ((type == TZ) || (type == DTZ))
		{
			*val = FROMVAL(tp);
		}
		else
		{
			*val = tp->value;
		}
	}

	return (type);
} /* DecodeUnits() */


/* datebsearch()
 * Binary search -- from Knuth (6.2.1) Algorithm B.  Special case like this
 * is WAY faster than the generic bsearch().
 */
static datetkn *
datebsearch(char *key, datetkn *base, unsigned int nel)
{
	register datetkn *last = base + nel - 1,
			   *position;
	register int result;

	while (last >= base)
	{
		position = base + ((last - base) >> 1);
		result = key[0] - position->token[0];
		if (result == 0)
		{
			result = strncmp(key, position->token, TOKMAXLEN);
			if (result == 0)
				return position;
		}
		if (result < 0)
			last = position - 1;
		else
			base = position + 1;
	}
	return NULL;
}


/* EncodeSpecialDateTime()
 * Convert reserved datetime data type to string.
 */
static int
EncodeSpecialDateTime(DateTime dt, char *str)
{
	if (DATETIME_IS_RESERVED(dt))
	{
		if (DATETIME_IS_INVALID(dt))
		{
			strcpy(str, INVALID);

		}
		else if (DATETIME_IS_NOBEGIN(dt))
		{
			strcpy(str, EARLY);

		}
		else if (DATETIME_IS_NOEND(dt))
		{
			strcpy(str, LATE);

		}
		else if (DATETIME_IS_CURRENT(dt))
		{
			strcpy(str, DCURRENT);

		}
		else if (DATETIME_IS_EPOCH(dt))
		{
			strcpy(str, EPOCH);

		}
		else
		{
#ifdef DATEDEBUG
			printf("EncodeSpecialDateTime- unrecognized date\n");
#endif
			strcpy(str, INVALID);
		}
		return (TRUE);
	}

	return (FALSE);
} /* EncodeSpecialDateTime() */


/* EncodeDateOnly()
 * Encode date as local time.
 */
int
EncodeDateOnly(struct tm * tm, int style, char *str)
{
	if ((tm->tm_mon < 1) || (tm->tm_mon > 12))
		return -1;

	switch (style)
	{
		/* compatible with ISO date formats */
		case USE_ISO_DATES:
			if (tm->tm_year > 0)
				sprintf(str, "%04d-%02d-%02d",
					tm->tm_year, tm->tm_mon, tm->tm_mday);
			else
				sprintf(str, "%04d-%02d-%02d %s",
					-(tm->tm_year - 1), tm->tm_mon, tm->tm_mday, "BC");
			break;

		/* compatible with Oracle/Ingres date formats */
		case USE_SQL_DATES:
			if (EuroDates)
				sprintf(str, "%02d/%02d", tm->tm_mday, tm->tm_mon);
			else
				sprintf(str, "%02d/%02d", tm->tm_mon, tm->tm_mday);
			if (tm->tm_year > 0)
				sprintf((str + 5), "/%04d", tm->tm_year);
			else
				sprintf((str + 5), "/%04d %s", -(tm->tm_year - 1), "BC");
			break;

		/* German-style date format */
		case USE_GERMAN_DATES:
			sprintf(str, "%02d.%02d", tm->tm_mday, tm->tm_mon);
			if (tm->tm_year > 0)
				sprintf((str + 5), ".%04d", tm->tm_year);
			else
				sprintf((str + 5), ".%04d %s", -(tm->tm_year - 1), "BC");
			break;

		/* traditional date-only style for Postgres */
		case USE_POSTGRES_DATES:
		default:
			if (EuroDates)
				sprintf(str, "%02d-%02d", tm->tm_mday, tm->tm_mon);
			else
				sprintf(str, "%02d-%02d", tm->tm_mon, tm->tm_mday);
			if (tm->tm_year > 0)
				sprintf((str + 5), "-%04d", tm->tm_year);
			else
				sprintf((str + 5), "-%04d %s", -(tm->tm_year - 1), "BC");
			break;
	}

#ifdef DATEDEBUG
	printf("EncodeDateOnly- date result is %s\n", str);
#endif

	return (TRUE);
} /* EncodeDateOnly() */


/* EncodeTimeOnly()
 * Encode time fields only.
 */
int
EncodeTimeOnly(struct tm * tm, double fsec, int style, char *str)
{
	double		sec;

	if ((tm->tm_hour < 0) || (tm->tm_hour > 24))
		return -1;

	sec = (tm->tm_sec + fsec);

	sprintf(str, "%02d:%02d:", tm->tm_hour, tm->tm_min);
	sprintf((str + 6), ((fsec != 0) ? "%05.2f" : "%02.0f"), sec);

#ifdef DATEDEBUG
	printf("EncodeTimeOnly- time result is %s\n", str);
#endif

	return (TRUE);
} /* EncodeTimeOnly() */


/* EncodeDateTime()
 * Encode date and time interpreted as local time.
 * Support several date styles:
 *  Postgres - day mon hh:mm:ss yyyy tz
 *  SQL - mm/dd/yyyy hh:mm:ss.ss tz
 *  ISO - yyyy-mm-dd hh:mm:ss+/-tz
 *  German - dd.mm/yyyy hh:mm:ss tz
 * Variants (affects order of month and day for Postgres and SQL styles):
 *  US - mm/dd/yyyy
 *  European - dd/mm/yyyy
 */
int
EncodeDateTime(struct tm * tm, double fsec, int *tzp, char **tzn, int style, char *str)
{
	int			day,
				hour,
				min;
	double		sec;

	if ((tm->tm_mon < 1) || (tm->tm_mon > 12))
		return -1;

	sec = (tm->tm_sec + fsec);

#ifdef DATEDEBUG
#ifdef USE_POSIX_TIME
#ifdef HAVE_INT_TIMEZONE
	printf("EncodeDateTime- timezone is %s (%s); offset is %d (%d); daylight is %d (%d)\n",
		   *tzn, tzname[0], *tzp, CTimeZone, tm->tm_isdst, CDayLight);
#else
	printf("EncodeDateTime- timezone is %s (%s); offset is %ld (%d); daylight is %d (%d)\n",
		   *tzn, tm->tm_zone, (-tm->tm_gmtoff), CTimeZone, tm->tm_isdst, CDayLight);
#endif
#else
	printf("EncodeDateTime- timezone is %s (%s); offset is %d; daylight is %d\n",
		   *tzn, CTZName, CTimeZone, CDayLight);
#endif
#endif

	switch (style)
	{
		/* compatible with ISO date formats */

		case USE_ISO_DATES:
			if (tm->tm_year > 0)
			{
				sprintf(str, "%04d-%02d-%02d %02d:%02d:",
					tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min);
				sprintf((str + 17), ((fsec != 0) ? "%05.2f" : "%02.0f"), sec);

				if ((*tzn != NULL) && (tm->tm_isdst >= 0))
				{
					if (tzp != NULL)
					{
						hour = -(*tzp / 3600);
						min = ((abs(*tzp) / 60) % 60);
					}
					else
					{
						hour = 0;
						min = 0;
					}
					sprintf((str + strlen(str)), ((min != 0) ? "%+03d:%02d" : "%+03d"), hour, min);
				}

			}
			else
			{
				if (tm->tm_hour || tm->tm_min)
					sprintf(str, "%04d-%02d-%02d %02d:%02d %s",
						-(tm->tm_year - 1), tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, "BC");
				else
					sprintf(str, "%04d-%02d-%02d %s",
						-(tm->tm_year - 1), tm->tm_mon, tm->tm_mday, "BC");
			}
			break;

		/* compatible with Oracle/Ingres date formats */
		case USE_SQL_DATES:
			if (EuroDates)
				sprintf(str, "%02d/%02d", tm->tm_mday, tm->tm_mon);
			else
				sprintf(str, "%02d/%02d", tm->tm_mon, tm->tm_mday);

			if (tm->tm_year > 0)
			{
				sprintf((str + 5), "/%04d %02d:%02d:%05.2f",
					tm->tm_year, tm->tm_hour, tm->tm_min, sec);

				if ((*tzn != NULL) && (tm->tm_isdst >= 0))
				{
					strcpy((str + 22), " ");
					strcpy((str + 23), *tzn);
				}

			}
			else
				sprintf((str + 5), "/%04d %02d:%02d %s",
					-(tm->tm_year - 1), tm->tm_hour, tm->tm_min, "BC");
			break;

		/* German variant on European style */
		case USE_GERMAN_DATES:
			sprintf(str, "%02d.%02d", tm->tm_mday, tm->tm_mon);
			if (tm->tm_year > 0)
			{
				sprintf((str + 5), ".%04d %02d:%02d:%05.2f",
					tm->tm_year, tm->tm_hour, tm->tm_min, sec);

				if ((*tzn != NULL) && (tm->tm_isdst >= 0))
				{
					strcpy((str + 22), " ");
					strcpy((str + 23), *tzn);
				}

			}
			else
				sprintf((str + 5), ".%04d %02d:%02d %s",
					-(tm->tm_year - 1), tm->tm_hour, tm->tm_min, "BC");
			break;

		/* backward-compatible with traditional Postgres abstime dates */
		case USE_POSTGRES_DATES:
		default:
			day = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
#ifdef DATEDEBUG
			printf("EncodeDateTime- day is %d\n", day);
#endif
			tm->tm_wday = j2day(day);

			strncpy(str, days[tm->tm_wday], 3);
			strcpy((str + 3), " ");

			if (EuroDates)
				sprintf((str + 4), "%02d %3s", tm->tm_mday, months[tm->tm_mon - 1]);
			else
				sprintf((str + 4), "%3s %02d", months[tm->tm_mon - 1], tm->tm_mday);

			if (tm->tm_year > 0)
			{
				sprintf((str + 10), " %02d:%02d", tm->tm_hour, tm->tm_min);
				if (fsec != 0)
				{
					sprintf((str + 16), ":%05.2f %04d", sec, tm->tm_year);
					if ((*tzn != NULL) && (tm->tm_isdst >= 0))
					{
						strcpy((str + 27), " ");
						strcpy((str + 28), *tzn);
					}
				}
				else
				{
					sprintf((str + 16), ":%02.0f %04d", sec, tm->tm_year);
					if ((*tzn != NULL) && (tm->tm_isdst >= 0))
					{
						strcpy((str + 24), " ");
						strcpy((str + 25), *tzn);
					}
				}

			}
			else
			{
				sprintf((str + 10), " %02d:%02d %04d %s",
					tm->tm_hour, tm->tm_min, -(tm->tm_year - 1), "BC");
			}
			break;
	}

#ifdef DATEDEBUG
	printf("EncodeDateTime- date result is %s\n", str);
#endif

	return (TRUE);
} /* EncodeDateTime() */


/* EncodeTimeSpan()
 * Interpret time structure as a delta time and convert to string.
 *
 * Pass a flag to specify the style of string, but only implement
 *	the traditional Postgres style for now. - tgl 97/03/27
 */
int
EncodeTimeSpan(struct tm * tm, double fsec, int style, char *str)
{
	int			is_before = FALSE;
	int			is_nonzero = FALSE;
	char	   *cp = str;

	switch (style)
	{
		/* compatible with ISO date formats */
		case USE_ISO_DATES:
		break;

		default:
			strcpy(cp, "@");
			cp += strlen(cp);
			break;
	}

	if (tm->tm_year != 0)
	{
		is_nonzero = TRUE;
		is_before |= (tm->tm_year < 0);
		sprintf(cp, " %d year%s", abs(tm->tm_year), ((abs(tm->tm_year) != 1) ? "s" : ""));
		cp += strlen(cp);
	}

	if (tm->tm_mon != 0)
	{
		is_nonzero = TRUE;
		is_before |= (tm->tm_mon < 0);
		sprintf(cp, " %d mon%s", abs(tm->tm_mon), ((abs(tm->tm_mon) != 1) ? "s" : ""));
		cp += strlen(cp);
	}

	if (tm->tm_mday != 0)
	{
		is_nonzero = TRUE;
		is_before |= (tm->tm_mday < 0);
		sprintf(cp, " %d day%s", abs(tm->tm_mday), ((abs(tm->tm_mday) != 1) ? "s" : ""));
		cp += strlen(cp);
	}

	switch (style)
	{
		/* compatible with ISO date formats */
		case USE_ISO_DATES:
			if ((tm->tm_hour != 0) || (tm->tm_min != 0))
				is_nonzero = TRUE;
			is_before |= ((tm->tm_hour < 0) || (tm->tm_min < 0));
			sprintf(cp, " %02d:%02d", abs(tm->tm_hour), abs(tm->tm_min));
			cp += strlen(cp);

			/* fractional seconds? */
			if (fsec != 0)
			{
				is_nonzero = TRUE;
				fsec += tm->tm_sec;
				is_before |= (fsec < 0);
				sprintf(cp, ":%05.2f", fabs(fsec));
				cp += strlen(cp);

				/* otherwise, integer seconds only? */
			}
			else if (tm->tm_sec != 0)
			{
				is_nonzero = TRUE;
				is_before |= (tm->tm_sec < 0);
				sprintf(cp, ":%02d", abs(tm->tm_sec));
				cp += strlen(cp);
			}
			break;

		case USE_POSTGRES_DATES:
		default:
			if (tm->tm_hour != 0)
			{
				is_nonzero = TRUE;
				is_before |= (tm->tm_hour < 0);
				sprintf(cp, " %d hour%s", abs(tm->tm_hour), ((abs(tm->tm_hour) != 1) ? "s" : ""));
				cp += strlen(cp);
			}

			if (tm->tm_min != 0)
			{
				is_nonzero = TRUE;
				is_before |= (tm->tm_min < 0);
				sprintf(cp, " %d min%s", abs(tm->tm_min), ((abs(tm->tm_min) != 1) ? "s" : ""));
				cp += strlen(cp);
			}

			/* fractional seconds? */
			if (fsec != 0)
			{
				is_nonzero = TRUE;
				fsec += tm->tm_sec;
				is_before |= (fsec < 0);
				sprintf(cp, " %.2f secs", fabs(fsec));
				cp += strlen(cp);

				/* otherwise, integer seconds only? */
			}
			else if (tm->tm_sec != 0)
			{
				is_nonzero = TRUE;
				is_before |= (tm->tm_sec < 0);
				sprintf(cp, " %d sec%s", abs(tm->tm_sec), ((abs(tm->tm_sec) != 1) ? "s" : ""));
				cp += strlen(cp);
			}
			break;
	}

	/* identically zero? then put in a unitless zero... */
	if (!is_nonzero)
	{
		strcat(cp, " 0");
		cp += strlen(cp);
	}

	if (is_before)
	{
		strcat(cp, " ago");
		cp += strlen(cp);
	}

#ifdef DATEDEBUG
	printf("EncodeTimeSpan- result is %s\n", str);
#endif

	return 0;
} /* EncodeTimeSpan() */


#if defined(linux) && defined(PPC)
int
datetime_is_epoch(double j)
{
	static union
	{
		double		epoch;
		unsigned char c[8];
	}			u;

	u.c[0] = 0x80;				/* sign bit */
	u.c[1] = 0x10;				/* DBL_MIN */

	return (j == u.epoch);
}
int
datetime_is_current(double j)
{
	static union
	{
		double		current;
		unsigned char c[8];
	}			u;

	u.c[1] = 0x10;				/* DBL_MIN */

	return (j == u.current);
}

#endif
