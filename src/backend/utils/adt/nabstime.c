/*
 * nabstime.c
 *	  Utilities for the built-in type "AbsoluteTime".
 *	  Functions for the built-in type "RelativeTime".
 *	  Functions for the built-in type "TimeInterval".
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/nabstime.c,v 1.67 2000/04/12 17:15:50 momjian Exp $
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>

#include "postgres.h"
#ifdef HAVE_FLOAT_H
#include <float.h>
#endif

#ifdef HAVE_LIMITS_H
#include <limits.h>
#ifndef MAXINT
#define MAXINT INT_MAX
#endif
#else
#ifdef HAVE_VALUES_H
#include <values.h>
#endif
#endif

#ifndef USE_POSIX_TIME
#include <sys/timeb.h>
#endif

#include "access/xact.h"
#include "miscadmin.h"
#include "utils/builtins.h"


#if 0
static AbsoluteTime tm2abstime(struct tm * tm, int tz);

#endif


#define MIN_DAYNUM -24856		/* December 13, 1901 */
#define MAX_DAYNUM 24854		/* January 18, 2038 */

#define INVALID_RELTIME_STR		"Undefined RelTime"
#define INVALID_RELTIME_STR_LEN (sizeof(INVALID_RELTIME_STR)-1)
#define RELTIME_LABEL			'@'
#define RELTIME_PAST			"ago"
#define DIRMAXLEN				(sizeof(RELTIME_PAST)-1)

/*
 * Unix epoch is Jan  1 00:00:00 1970.
 * Postgres knows about times sixty-eight years on either side of that
 * for these 4-byte types.
 *
 * "tinterval" is two 4-byte fields.
 * Definitions for parsing tinterval.
 */

#define IsSpace(C)				((C) == ' ')

#define T_INTERVAL_INVAL   0	/* data represents no valid interval */
#define T_INTERVAL_VALID   1	/* data represents a valid interval */
/*
 * ['Mon May 10 23:59:12 1943 PST' 'Sun Jan 14 03:14:21 1973 PST']
 * 0		1		  2			3		  4			5		  6
 * 1234567890123456789012345678901234567890123456789012345678901234
 *
 * we allocate some extra -- timezones are usually 3 characters but
 * this is not in the POSIX standard...
 */
#define T_INTERVAL_LEN					80
#define INVALID_INTERVAL_STR			"Undefined Range"
#define INVALID_INTERVAL_STR_LEN		(sizeof(INVALID_INTERVAL_STR)-1)

#define ABSTIMEMIN(t1, t2) abstimele((t1),(t2)) ? (t1) : (t2)
#define ABSTIMEMAX(t1, t2) abstimelt((t1),(t2)) ? (t2) : (t1)

#ifdef NOT_USED
static char *unit_tab[] = {
	"second", "seconds", "minute", "minutes",
	"hour", "hours", "day", "days", "week", "weeks",
"month", "months", "year", "years"};

#define UNITMAXLEN 7			/* max length of a unit name */
#define NUNITS 14				/* number of different units */

/* table of seconds per unit (month = 30 days, year = 365 days)  */
static int	sec_tab[] = {
	1, 1, 60, 60,
	3600, 3600, 86400, 86400, 604800, 604800,
2592000, 2592000, 31536000, 31536000};

#endif

/*
 * Function prototypes -- internal to this file only
 */

static void reltime2tm(RelativeTime time, struct tm * tm);

#ifdef NOT_USED
static int	correct_unit(char *unit, int *unptr);
static int	correct_dir(char *direction, int *signptr);

#endif

static int istinterval(char *i_string,
			AbsoluteTime *i_start,
			AbsoluteTime *i_end);

/* GetCurrentAbsoluteTime()
 * Get the current system time. Set timezone parameters if not specified elsewhere.
 * Define HasTZSet to allow clients to specify the default timezone.
 *
 * Returns the number of seconds since epoch (January 1 1970 GMT)
 */
AbsoluteTime
GetCurrentAbsoluteTime(void)
{
	time_t		now;

#ifdef USE_POSIX_TIME
	struct tm  *tm;

	now = time(NULL);
#else							/* ! USE_POSIX_TIME */
	struct timeb tb;			/* the old V7-ism */

	ftime(&tb);
	now = tb.time;
#endif

	if (!HasCTZSet)
	{
#ifdef USE_POSIX_TIME
#if defined(HAVE_TM_ZONE)
		tm = localtime(&now);

		CTimeZone = -tm->tm_gmtoff;		/* tm_gmtoff is Sun/DEC-ism */
		CDayLight = (tm->tm_isdst > 0);

#ifdef NOT_USED

		/*
		 * XXX is there a better way to get local timezone string w/o
		 * tzname? - tgl 97/03/18
		 */
		strftime(CTZName, MAXTZLEN, "%Z", tm);
#endif

		/*
		 * XXX FreeBSD man pages indicate that this should work - thomas
		 * 1998-12-12
		 */
		strcpy(CTZName, tm->tm_zone);

#elif defined(HAVE_INT_TIMEZONE)
		tm = localtime(&now);

		CDayLight = tm->tm_isdst;
		CTimeZone =
#ifdef __CYGWIN32__
			(tm->tm_isdst ? (_timezone - 3600) : _timezone);
#else
			(tm->tm_isdst ? (timezone - 3600) : timezone);
#endif
		strcpy(CTZName, tzname[tm->tm_isdst]);
#else
#error USE_POSIX_TIME defined but no time zone available
#endif
#else							/* ! USE_POSIX_TIME */
		CTimeZone = tb.timezone * 60;
		CDayLight = (tb.dstflag != 0);

		/*
		 * XXX does this work to get the local timezone string in V7? -
		 * tgl 97/03/18
		 */
		strftime(CTZName, MAXTZLEN, "%Z", localtime(&now));
#endif
	};

	return (AbsoluteTime) now;
}	/* GetCurrentAbsoluteTime() */


void
GetCurrentTime(struct tm * tm)
{
	int			tz;

	abstime2tm(GetCurrentTransactionStartTime(), &tz, tm, NULL);

	return;
}	/* GetCurrentTime() */


void
abstime2tm(AbsoluteTime time, int *tzp, struct tm * tm, char *tzn)
{
#ifdef USE_POSIX_TIME
	struct tm  *tx;

#else							/* ! USE_POSIX_TIME */
	struct timeb tb;			/* the old V7-ism */

	ftime(&tb);
#endif

#ifdef USE_POSIX_TIME
	if (tzp != NULL)
		tx = localtime((time_t *) &time);
	else
	{
		tx = gmtime((time_t *) &time);
	};
#endif

#ifdef USE_POSIX_TIME

	tm->tm_year = tx->tm_year + 1900;
	tm->tm_mon = tx->tm_mon + 1;
	tm->tm_mday = tx->tm_mday;
	tm->tm_hour = tx->tm_hour;
	tm->tm_min = tx->tm_min;
	tm->tm_sec = tx->tm_sec;
	tm->tm_isdst = tx->tm_isdst;

#if defined(HAVE_TM_ZONE)
	tm->tm_gmtoff = tx->tm_gmtoff;
	tm->tm_zone = tx->tm_zone;

	if (tzp != NULL)
		*tzp = -tm->tm_gmtoff;	/* tm_gmtoff is Sun/DEC-ism */
	/* XXX FreeBSD man pages indicate that this should work - tgl 97/04/23 */
	if (tzn != NULL)
	{

		/*
		 * Copy no more than MAXTZLEN bytes of timezone to tzn, in case it
		 * contains an error message, which doesn't fit in the buffer
		 */
		strncpy(tzn, tm->tm_zone, MAXTZLEN);
		if (strlen(tm->tm_zone) > MAXTZLEN)
		{
			tzn[MAXTZLEN] = '\0';
			elog(NOTICE, "Invalid timezone \'%s\'", tm->tm_zone);
		}
	}
#elif defined(HAVE_INT_TIMEZONE)
	if (tzp != NULL)
#ifdef __CYGWIN__
		*tzp = (tm->tm_isdst ? (_timezone - 3600) : _timezone);
#else
		*tzp = (tm->tm_isdst ? (timezone - 3600) : timezone);
#endif
	if (tzn != NULL)
	{

		/*
		 * Copy no more than MAXTZLEN bytes of timezone to tzn, in case it
		 * contains an error message, which doesn't fit in the buffer
		 */
		strncpy(tzn, tzname[tm->tm_isdst], MAXTZLEN);
		if (strlen(tzname[tm->tm_isdst]) > MAXTZLEN)
		{
			tzn[MAXTZLEN] = '\0';
			elog(NOTICE, "Invalid timezone \'%s\'", tzname[tm->tm_isdst]);
		}
	}
#else
#error POSIX time support is broken
#endif
#else							/* ! USE_POSIX_TIME */
	if (tzp != NULL)
		*tzp = tb.timezone * 60;

	/*
	 * XXX does this work to get the local timezone string in V7? - tgl
	 * 97/03/18
	 */
	if (tzn != NULL)
		strftime(tzn, MAXTZLEN, "%Z", localtime(&now));
#endif

	return;
}	/* abstime2tm() */


/* tm2abstime()
 * Convert a tm structure to abstime.
 * Note that tm has full year (not 1900-based) and 1-based month.
 */
static AbsoluteTime
tm2abstime(struct tm * tm, int tz)
{
	int			day,
				sec;

	/* validate, before going out of range on some members */
	if (tm->tm_year < 1901 || tm->tm_year > 2038
		|| tm->tm_mon < 1 || tm->tm_mon > 12
		|| tm->tm_mday < 1 || tm->tm_mday > 31
		|| tm->tm_hour < 0 || tm->tm_hour >= 24
		|| tm->tm_min < 0 || tm->tm_min > 59
		|| tm->tm_sec < 0 || tm->tm_sec > 59)
		return INVALID_ABSTIME;

	day = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(1970, 1, 1));

	/* check for time out of range */
	if ((day < MIN_DAYNUM) || (day > MAX_DAYNUM))
		return INVALID_ABSTIME;

	/* convert to seconds */
	sec = tm->tm_sec + tz + (tm->tm_min + (day * 24 + tm->tm_hour) * 60) * 60;

	/* check for overflow */
	if ((day == MAX_DAYNUM && sec < 0) ||
		(day == MIN_DAYNUM && sec > 0))
		return INVALID_ABSTIME;

	/* check for reserved values (e.g. "current" on edge of usual range */
	if (!AbsoluteTimeIsReal(sec))
		return INVALID_ABSTIME;

	return sec;
}	/* tm2abstime() */


/* nabstimein()
 * Decode date/time string and return abstime.
 */
AbsoluteTime
nabstimein(char *str)
{
	AbsoluteTime result;

	double		fsec;
	int			tz = 0;
	struct tm	date,
			   *tm = &date;

	char	   *field[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + 1];
	int			dtype;
	int			nf,
				ftype[MAXDATEFIELDS];

	if (!PointerIsValid(str))
		elog(ERROR, "Bad (null) abstime external representation");

	if (strlen(str) > MAXDATELEN)
		elog(ERROR, "Bad (length) abstime external representation '%s'", str);

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
	  || (DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz) != 0))
		elog(ERROR, "Bad abstime external representation '%s'", str);

	switch (dtype)
	{
		case DTK_DATE:
			result = tm2abstime(tm, tz);
			break;

		case DTK_EPOCH:
			result = EPOCH_ABSTIME;
			break;

		case DTK_CURRENT:
			result = CURRENT_ABSTIME;
			break;

		case DTK_LATE:
			result = NOEND_ABSTIME;
			break;

		case DTK_EARLY:
			result = NOSTART_ABSTIME;
			break;

		case DTK_INVALID:
			result = INVALID_ABSTIME;
			break;

		default:
			elog(ERROR, "Bad abstime (internal coding error) '%s'", str);
			result = INVALID_ABSTIME;
			break;
	};

	return result;
}	/* nabstimein() */


/* nabstimeout()
 * Given an AbsoluteTime return the English text version of the date
 */
char *
nabstimeout(AbsoluteTime time)
{
	char	   *result;
	int			tz;
	double		fsec = 0;
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];
	char		zone[MAXDATELEN + 1],
			   *tzn = zone;

	switch (time)
	{
		case EPOCH_ABSTIME:
			strcpy(buf, EPOCH);
			break;
		case INVALID_ABSTIME:
			strcpy(buf, INVALID);
			break;
		case CURRENT_ABSTIME:
			strcpy(buf, DCURRENT);
			break;
		case NOEND_ABSTIME:
			strcpy(buf, LATE);
			break;
		case NOSTART_ABSTIME:
			strcpy(buf, EARLY);
			break;
		default:
			abstime2tm(time, &tz, tm, tzn);
			EncodeDateTime(tm, fsec, &tz, &tzn, DateStyle, buf);
			break;
	}

	result = palloc(strlen(buf) + 1);
	strcpy(result, buf);

	return result;
}	/* nabstimeout() */


/*
 *	AbsoluteTimeIsBefore -- true iff time1 is before time2.
 *	AbsoluteTimeIsBefore -- true iff time1 is after time2.
 */
bool
AbsoluteTimeIsBefore(AbsoluteTime time1, AbsoluteTime time2)
{
	Assert(AbsoluteTimeIsValid(time1));
	Assert(AbsoluteTimeIsValid(time2));

	if (time1 == CURRENT_ABSTIME)
		time1 = GetCurrentTransactionStartTime();

	if (time2 == CURRENT_ABSTIME)
		time2 = GetCurrentTransactionStartTime();

	return time1 < time2;
}

#ifdef NOT_USED
bool
AbsoluteTimeIsAfter(AbsoluteTime time1, AbsoluteTime time2)
{
	Assert(AbsoluteTimeIsValid(time1));
	Assert(AbsoluteTimeIsValid(time2));

	if (time1 == CURRENT_ABSTIME)
		time1 = GetCurrentTransactionStartTime();

	if (time2 == CURRENT_ABSTIME)
		time2 = GetCurrentTransactionStartTime();

	return time1 > time2;
}

#endif

/* abstime_finite()
 */
bool
abstime_finite(AbsoluteTime abstime)
{
	return ((abstime != INVALID_ABSTIME)
		  && (abstime != NOSTART_ABSTIME) && (abstime != NOEND_ABSTIME));
}	/* abstime_finite() */


/*
 *		abstimeeq		- returns 1, iff arguments are equal
 *		abstimene		- returns 1, iff arguments are not equal
 *		abstimelt		- returns 1, iff t1 less than t2
 *		abstimegt		- returns 1, iff t1 greater than t2
 *		abstimele		- returns 1, iff t1 less than or equal to t2
 *		abstimege		- returns 1, iff t1 greater than or equal to t2
 */
bool
abstimeeq(AbsoluteTime t1, AbsoluteTime t2)
{
	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		return FALSE;
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	return t1 == t2;
}

bool
abstimene(AbsoluteTime t1, AbsoluteTime t2)
{
	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		return FALSE;
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	return t1 != t2;
}

bool
abstimelt(AbsoluteTime t1, AbsoluteTime t2)
{
	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		return FALSE;
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	return t1 < t2;
}

bool
abstimegt(AbsoluteTime t1, AbsoluteTime t2)
{
	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		return FALSE;
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	return t1 > t2;
}

bool
abstimele(AbsoluteTime t1, AbsoluteTime t2)
{
	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		return FALSE;
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	return t1 <= t2;
}

bool
abstimege(AbsoluteTime t1, AbsoluteTime t2)
{
	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		return FALSE;
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	return t1 >= t2;
}


/* datetime_abstime()
 * Convert timestamp to abstime.
 */
AbsoluteTime
timestamp_abstime(Timestamp *timestamp)
{
	AbsoluteTime result;

	double		fsec;
	struct tm	tt,
			   *tm = &tt;

	if (!PointerIsValid(timestamp))
	{
		result = INVALID_ABSTIME;

	}
	else if (TIMESTAMP_IS_INVALID(*timestamp))
	{
		result = INVALID_ABSTIME;

	}
	else if (TIMESTAMP_IS_NOBEGIN(*timestamp))
	{
		result = NOSTART_ABSTIME;

	}
	else if (TIMESTAMP_IS_NOEND(*timestamp))
	{
		result = NOEND_ABSTIME;

	}
	else
	{
		if (TIMESTAMP_IS_RELATIVE(*timestamp))
		{
			timestamp2tm(SetTimestamp(*timestamp), NULL, tm, &fsec, NULL);
			result = tm2abstime(tm, 0);

		}
		else if (timestamp2tm(*timestamp, NULL, tm, &fsec, NULL) == 0)
		{
			result = tm2abstime(tm, 0);

		}
		else
		{
			result = INVALID_ABSTIME;
		};
	};

	return result;
}	/* timestamp_abstime() */

/* abstime_timestamp()
 * Convert abstime to timestamp.
 */
Timestamp  *
abstime_timestamp(AbsoluteTime abstime)
{
	Timestamp  *result;

	if (!PointerIsValid(result = palloc(sizeof(Timestamp))))
		elog(ERROR, "Unable to allocate space to convert abstime to timestamp");

	switch (abstime)
	{
		case INVALID_ABSTIME:
			TIMESTAMP_INVALID(*result);
			break;

		case NOSTART_ABSTIME:
			TIMESTAMP_NOBEGIN(*result);
			break;

		case NOEND_ABSTIME:
			TIMESTAMP_NOEND(*result);
			break;

		case EPOCH_ABSTIME:
			TIMESTAMP_EPOCH(*result);
			break;

		case CURRENT_ABSTIME:
			TIMESTAMP_CURRENT(*result);
			break;

		default:
			*result = abstime + ((date2j(1970, 1, 1) - date2j(2000, 1, 1)) * 86400);
			break;
	};

	return result;
}	/* abstime_timestamp() */


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		reltimein		- converts a reltime string in an internal format
 */
RelativeTime
reltimein(char *str)
{
	RelativeTime result;

	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	int			dtype;
	char	   *field[MAXDATEFIELDS];
	int			nf,
				ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + 1];

	if (!PointerIsValid(str))
		elog(ERROR, "Bad (null) date external representation");

	if (strlen(str) > MAXDATELEN)
		elog(ERROR, "Bad (length) reltime external representation '%s'", str);

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
		|| (DecodeDateDelta(field, ftype, nf, &dtype, tm, &fsec) != 0))
		elog(ERROR, "Bad reltime external representation '%s'", str);

	switch (dtype)
	{
		case DTK_DELTA:
			result = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec);
			result += (((tm->tm_year * 365) + (tm->tm_mon * 30) + tm->tm_mday) * (24 * 60 * 60));
			return result;

		default:
			return INVALID_RELTIME;
	}

	elog(ERROR, "Bad reltime (internal coding error) '%s'", str);
	return INVALID_RELTIME;
}	/* reltimein() */


/*
 *		reltimeout		- converts the internal format to a reltime string
 */
char *
reltimeout(RelativeTime time)
{
	char	   *result;
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];

	if (time == INVALID_RELTIME)
	{
		strcpy(buf, INVALID_RELTIME_STR);

	}
	else
	{
		reltime2tm(time, tm);
		EncodeTimeSpan(tm, 0, DateStyle, buf);
	}

	result = palloc(strlen(buf) + 1);
	strcpy(result, buf);

	return result;
}	/* reltimeout() */


static void
reltime2tm(RelativeTime time, struct tm * tm)
{
	TMODULO(time, tm->tm_year, 31536000);
	TMODULO(time, tm->tm_mon, 2592000);
	TMODULO(time, tm->tm_mday, 86400);
	TMODULO(time, tm->tm_hour, 3600);
	TMODULO(time, tm->tm_min, 60);
	TMODULO(time, tm->tm_sec, 1);

	return;
}	/* reltime2tm() */

#ifdef NOT_USED
int
dummyfunc()
{
	char	   *timestring;
	long		quantity;
	int			i;
	int			unitnr;

	timestring = (char *) palloc(Max(strlen(INVALID_RELTIME_STR),
									 UNITMAXLEN) + 1);
	if (timevalue == INVALID_RELTIME)
	{
		strcpy(timestring, INVALID_RELTIME_STR);
		return timestring;
	}

	if (timevalue == 0)
		i = 1;					/* unit = 'seconds' */
	else
		for (i = 12; i >= 0; i = i - 2)
			if ((timevalue % sec_tab[i]) == 0)
				break;			/* appropriate unit found */
	unitnr = i;
	quantity = (timevalue / sec_tab[unitnr]);
	if (quantity > 1 || quantity < -1)
		unitnr++;				/* adjust index for PLURAL of unit */
	if (quantity >= 0)
		sprintf(timestring, "%c %lu %s", RELTIME_LABEL,
				quantity, unit_tab[unitnr]);
	else
		sprintf(timestring, "%c %lu %s %s", RELTIME_LABEL,
				(quantity * -1), unit_tab[unitnr], RELTIME_PAST);
	return timestring;
}

#endif


/*
 *		tintervalin		- converts an interval string to an internal format
 */
TimeInterval
tintervalin(char *intervalstr)
{
	int			error;
	AbsoluteTime i_start,
				i_end,
				t1,
				t2;
	TimeInterval interval;

	interval = (TimeInterval) palloc(sizeof(TimeIntervalData));
	error = istinterval(intervalstr, &t1, &t2);
	if (error == 0)
		interval->status = T_INTERVAL_INVAL;
	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		interval->status = T_INTERVAL_INVAL;	/* undefined  */
	else
	{
		i_start = ABSTIMEMIN(t1, t2);
		i_end = ABSTIMEMAX(t1, t2);
		interval->data[0] = i_start;
		interval->data[1] = i_end;
		interval->status = T_INTERVAL_VALID;
	}
	return interval;
}


/*
 *		tintervalout	- converts an internal interval format to a string
 *
 */
char *
tintervalout(TimeInterval interval)
{
	char	   *i_str,
			   *p;

	i_str = (char *) palloc(T_INTERVAL_LEN);	/* ['...' '...'] */
	strcpy(i_str, "[\"");
	if (interval->status == T_INTERVAL_INVAL)
		strcat(i_str, INVALID_INTERVAL_STR);
	else
	{
		p = nabstimeout(interval->data[0]);
		strcat(i_str, p);
		pfree(p);
		strcat(i_str, "\" \"");
		p = nabstimeout(interval->data[1]);
		strcat(i_str, p);
		pfree(p);
	}
	strcat(i_str, "\"]\0");
	return i_str;
}


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

RelativeTime
interval_reltime(Interval *interval)
{
	RelativeTime time;
	int			year,
				month;
	double		span;

	if (!PointerIsValid(interval))
		time = INVALID_RELTIME;

	if (INTERVAL_IS_INVALID(*interval))
	{
		time = INVALID_RELTIME;

	}
	else
	{
		if (interval->month == 0)
		{
			year = 0;
			month = 0;

		}
		else if (abs(interval->month) >= 12)
		{
			year = (interval->month / 12);
			month = (interval->month % 12);

		}
		else
		{
			year = 0;
			month = interval->month;
		}

		span = (((((double) 365 * year) + ((double) 30 * month)) * 86400) + interval->time);

		time = (((span > INT_MIN) && (span < INT_MAX)) ? span : INVALID_RELTIME);
	}

	return time;
}	/* interval_reltime() */


Interval   *
reltime_interval(RelativeTime reltime)
{
	Interval   *result;
	int			year,
				month;

	if (!PointerIsValid(result = palloc(sizeof(Interval))))
		elog(ERROR, "Memory allocation failed, can't convert reltime to interval");

	switch (reltime)
	{
		case INVALID_RELTIME:
			INTERVAL_INVALID(*result);
			break;

		default:
			TMODULO(reltime, year, 31536000);
			TMODULO(reltime, month, 2592000);

			result->time = reltime;
			result->month = ((12 * year) + month);
	}

	return result;
}	/* reltime_interval() */


/*
 *		mktinterval		- creates a time interval with endpoints t1 and t2
 */
TimeInterval
mktinterval(AbsoluteTime t1, AbsoluteTime t2)
{
	AbsoluteTime tstart = ABSTIMEMIN(t1, t2),
				tend = ABSTIMEMAX(t1, t2);
	TimeInterval interval;

	interval = (TimeInterval) palloc(sizeof(TimeIntervalData));
	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		interval->status = T_INTERVAL_INVAL;
	else
	{
		interval->status = T_INTERVAL_VALID;
		interval->data[0] = tstart;
		interval->data[1] = tend;
	}

	return interval;
}

/*
 *		timepl, timemi and abstimemi use the formula
 *				abstime + reltime = abstime
 *		so		abstime - reltime = abstime
 *		and		abstime - abstime = reltime
 */

/*
 *		timepl			- returns the value of (abstime t1 + relime t2)
 */
AbsoluteTime
timepl(AbsoluteTime t1, RelativeTime t2)
{
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();

	if (AbsoluteTimeIsReal(t1) &&
		RelativeTimeIsValid(t2) &&
		((t2 > 0) ? (t1 < NOEND_ABSTIME - t2)
		 : (t1 > NOSTART_ABSTIME - t2)))		/* prevent overflow */
		return t1 + t2;

	return INVALID_ABSTIME;
}


/*
 *		timemi			- returns the value of (abstime t1 - reltime t2)
 */
AbsoluteTime
timemi(AbsoluteTime t1, RelativeTime t2)
{
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();

	if (AbsoluteTimeIsReal(t1) &&
		RelativeTimeIsValid(t2) &&
		((t2 > 0) ? (t1 > NOSTART_ABSTIME + t2)
		 : (t1 < NOEND_ABSTIME + t2)))	/* prevent overflow */
		return t1 - t2;

	return INVALID_ABSTIME;
}


/*
 *		abstimemi		- returns the value of (abstime t1 - abstime t2)
 */
static RelativeTime
abstimemi(AbsoluteTime t1, AbsoluteTime t2)
{
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	if (AbsoluteTimeIsReal(t1) &&
		AbsoluteTimeIsReal(t2))
		return t1 - t2;

	return INVALID_RELTIME;
}


/*
 *		intinterval		- returns 1, iff absolute date is in the interval
 */
int
intinterval(AbsoluteTime t, TimeInterval interval)
{
	if (interval->status == T_INTERVAL_VALID && t != INVALID_ABSTIME)
		return (abstimege(t, interval->data[0]) &&
				abstimele(t, interval->data[1]));
	return 0;
}

/*
 *		tintervalrel		- returns  relative time corresponding to interval
 */
RelativeTime
tintervalrel(TimeInterval interval)
{
	if (interval->status == T_INTERVAL_VALID)
		return abstimemi(interval->data[1], interval->data[0]);
	else
		return INVALID_RELTIME;
}

/*
 *		timenow			- returns  time "now", internal format
 *
 *		Now AbsoluteTime is time since Jan 1 1970 -mer 7 Feb 1992
 */
AbsoluteTime
timenow()
{
	time_t		sec;

	if (time(&sec) < 0)
		return INVALID_ABSTIME;
	return (AbsoluteTime) sec;
}

/*
 *		reltimeeq		- returns 1, iff arguments are equal
 *		reltimene		- returns 1, iff arguments are not equal
 *		reltimelt		- returns 1, iff t1 less than t2
 *		reltimegt		- returns 1, iff t1 greater than t2
 *		reltimele		- returns 1, iff t1 less than or equal to t2
 *		reltimege		- returns 1, iff t1 greater than or equal to t2
 */
bool
reltimeeq(RelativeTime t1, RelativeTime t2)
{
	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		return 0;
	return t1 == t2;
}

bool
reltimene(RelativeTime t1, RelativeTime t2)
{
	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		return 0;
	return t1 != t2;
}

bool
reltimelt(RelativeTime t1, RelativeTime t2)
{
	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		return 0;
	return t1 < t2;
}

bool
reltimegt(RelativeTime t1, RelativeTime t2)
{
	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		return 0;
	return t1 > t2;
}

bool
reltimele(RelativeTime t1, RelativeTime t2)
{
	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		return 0;
	return t1 <= t2;
}

bool
reltimege(RelativeTime t1, RelativeTime t2)
{
	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		return 0;
	return t1 >= t2;
}


/*
 *		tintervalsame	- returns 1, iff interval i1 is same as interval i2
 *		Check begin and end time.
 */
bool
tintervalsame(TimeInterval i1, TimeInterval i2)
{
	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		return FALSE;			/* invalid interval */
	return (abstimeeq(i1->data[0], i2->data[0]) &&
			abstimeeq(i1->data[1], i2->data[1]));
}	/* tintervalsame() */


/*
 *		tintervaleq		- returns 1, iff interval i1 is equal to interval i2
 *		Check length of intervals.
 */
bool
tintervaleq(TimeInterval i1, TimeInterval i2)
{
	AbsoluteTime t10,
				t11,
				t20,
				t21;

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		return FALSE;			/* invalid interval */

	t10 = i1->data[0];
	t11 = i1->data[1];
	t20 = i2->data[0];
	t21 = i2->data[1];

	if ((t10 == INVALID_ABSTIME) || (t20 == INVALID_ABSTIME)
		|| (t20 == INVALID_ABSTIME) || (t21 == INVALID_ABSTIME))
		return FALSE;

	if (t10 == CURRENT_ABSTIME)
		t10 = GetCurrentTransactionStartTime();
	if (t11 == CURRENT_ABSTIME)
		t11 = GetCurrentTransactionStartTime();
	if (t20 == CURRENT_ABSTIME)
		t20 = GetCurrentTransactionStartTime();
	if (t21 == CURRENT_ABSTIME)
		t21 = GetCurrentTransactionStartTime();

	return (t11 - t10) == (t21 - t20);
}	/* tintervaleq() */

/*
 *		tintervalne		- returns 1, iff interval i1 is not equal to interval i2
 *		Check length of intervals.
 */
bool
tintervalne(TimeInterval i1, TimeInterval i2)
{
	AbsoluteTime t10,
				t11,
				t20,
				t21;

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		return FALSE;			/* invalid interval */

	t10 = i1->data[0];
	t11 = i1->data[1];
	t20 = i2->data[0];
	t21 = i2->data[1];

	if ((t10 == INVALID_ABSTIME) || (t20 == INVALID_ABSTIME)
		|| (t20 == INVALID_ABSTIME) || (t21 == INVALID_ABSTIME))
		return FALSE;

	if (t10 == CURRENT_ABSTIME)
		t10 = GetCurrentTransactionStartTime();
	if (t11 == CURRENT_ABSTIME)
		t11 = GetCurrentTransactionStartTime();
	if (t20 == CURRENT_ABSTIME)
		t20 = GetCurrentTransactionStartTime();
	if (t21 == CURRENT_ABSTIME)
		t21 = GetCurrentTransactionStartTime();

	return (t11 - t10) != (t21 - t20);
}	/* tintervalne() */

/*
 *		tintervallt		- returns TRUE, iff interval i1 is less than interval i2
 *		Check length of intervals.
 */
bool
tintervallt(TimeInterval i1, TimeInterval i2)
{
	AbsoluteTime t10,
				t11,
				t20,
				t21;

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		return FALSE;			/* invalid interval */

	t10 = i1->data[0];
	t11 = i1->data[1];
	t20 = i2->data[0];
	t21 = i2->data[1];

	if ((t10 == INVALID_ABSTIME) || (t20 == INVALID_ABSTIME)
		|| (t20 == INVALID_ABSTIME) || (t21 == INVALID_ABSTIME))
		return FALSE;

	if (t10 == CURRENT_ABSTIME)
		t10 = GetCurrentTransactionStartTime();
	if (t11 == CURRENT_ABSTIME)
		t11 = GetCurrentTransactionStartTime();
	if (t20 == CURRENT_ABSTIME)
		t20 = GetCurrentTransactionStartTime();
	if (t21 == CURRENT_ABSTIME)
		t21 = GetCurrentTransactionStartTime();

	return (t11 - t10) < (t21 - t20);
}	/* tintervallt() */

/*
 *		tintervalle		- returns TRUE, iff interval i1 is less than or equal to interval i2
 *		Check length of intervals.
 */
bool
tintervalle(TimeInterval i1, TimeInterval i2)
{
	AbsoluteTime t10,
				t11,
				t20,
				t21;

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		return FALSE;			/* invalid interval */

	t10 = i1->data[0];
	t11 = i1->data[1];
	t20 = i2->data[0];
	t21 = i2->data[1];

	if ((t10 == INVALID_ABSTIME) || (t20 == INVALID_ABSTIME)
		|| (t20 == INVALID_ABSTIME) || (t21 == INVALID_ABSTIME))
		return FALSE;

	if (t10 == CURRENT_ABSTIME)
		t10 = GetCurrentTransactionStartTime();
	if (t11 == CURRENT_ABSTIME)
		t11 = GetCurrentTransactionStartTime();
	if (t20 == CURRENT_ABSTIME)
		t20 = GetCurrentTransactionStartTime();
	if (t21 == CURRENT_ABSTIME)
		t21 = GetCurrentTransactionStartTime();

	return (t11 - t10) <= (t21 - t20);
}	/* tintervalle() */

/*
 *		tintervalgt		- returns TRUE, iff interval i1 is less than interval i2
 *		Check length of intervals.
 */
bool
tintervalgt(TimeInterval i1, TimeInterval i2)
{
	AbsoluteTime t10,
				t11,
				t20,
				t21;

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		return FALSE;			/* invalid interval */

	t10 = i1->data[0];
	t11 = i1->data[1];
	t20 = i2->data[0];
	t21 = i2->data[1];

	if ((t10 == INVALID_ABSTIME) || (t20 == INVALID_ABSTIME)
		|| (t20 == INVALID_ABSTIME) || (t21 == INVALID_ABSTIME))
		return FALSE;

	if (t10 == CURRENT_ABSTIME)
		t10 = GetCurrentTransactionStartTime();
	if (t11 == CURRENT_ABSTIME)
		t11 = GetCurrentTransactionStartTime();
	if (t20 == CURRENT_ABSTIME)
		t20 = GetCurrentTransactionStartTime();
	if (t21 == CURRENT_ABSTIME)
		t21 = GetCurrentTransactionStartTime();

	return (t11 - t10) > (t21 - t20);
}	/* tintervalgt() */

/*
 *		tintervalge		- returns TRUE, iff interval i1 is less than or equal to interval i2
 *		Check length of intervals.
 */
bool
tintervalge(TimeInterval i1, TimeInterval i2)
{
	AbsoluteTime t10,
				t11,
				t20,
				t21;

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		return FALSE;			/* invalid interval */

	t10 = i1->data[0];
	t11 = i1->data[1];
	t20 = i2->data[0];
	t21 = i2->data[1];

	if ((t10 == INVALID_ABSTIME) || (t20 == INVALID_ABSTIME)
		|| (t20 == INVALID_ABSTIME) || (t21 == INVALID_ABSTIME))
		return FALSE;

	if (t10 == CURRENT_ABSTIME)
		t10 = GetCurrentTransactionStartTime();
	if (t11 == CURRENT_ABSTIME)
		t11 = GetCurrentTransactionStartTime();
	if (t20 == CURRENT_ABSTIME)
		t20 = GetCurrentTransactionStartTime();
	if (t21 == CURRENT_ABSTIME)
		t21 = GetCurrentTransactionStartTime();

	return (t11 - t10) >= (t21 - t20);
}	/* tintervalge() */


/*
 *		tintervalleneq	- returns 1, iff length of interval i is equal to
 *								reltime t
 */
bool
tintervalleneq(TimeInterval i, RelativeTime t)
{
	RelativeTime rt;

	if ((i->status == T_INTERVAL_INVAL) || (t == INVALID_RELTIME))
		return 0;
	rt = tintervalrel(i);
	return rt != INVALID_RELTIME && rt == t;
}

/*
 *		tintervallenne	- returns 1, iff length of interval i is not equal
 *								to reltime t
 */
bool
tintervallenne(TimeInterval i, RelativeTime t)
{
	RelativeTime rt;

	if ((i->status == T_INTERVAL_INVAL) || (t == INVALID_RELTIME))
		return 0;
	rt = tintervalrel(i);
	return rt != INVALID_RELTIME && rt != t;
}

/*
 *		tintervallenlt	- returns 1, iff length of interval i is less than
 *								reltime t
 */
bool
tintervallenlt(TimeInterval i, RelativeTime t)
{
	RelativeTime rt;

	if ((i->status == T_INTERVAL_INVAL) || (t == INVALID_RELTIME))
		return 0;
	rt = tintervalrel(i);
	return rt != INVALID_RELTIME && rt < t;
}

/*
 *		tintervallengt	- returns 1, iff length of interval i is greater than
 *								reltime t
 */
bool
tintervallengt(TimeInterval i, RelativeTime t)
{
	RelativeTime rt;

	if ((i->status == T_INTERVAL_INVAL) || (t == INVALID_RELTIME))
		return 0;
	rt = tintervalrel(i);
	return rt != INVALID_RELTIME && rt > t;
}

/*
 *		tintervallenle	- returns 1, iff length of interval i is less or equal
 *									than reltime t
 */
bool
tintervallenle(TimeInterval i, RelativeTime t)
{
	RelativeTime rt;

	if ((i->status == T_INTERVAL_INVAL) || (t == INVALID_RELTIME))
		return 0;
	rt = tintervalrel(i);
	return rt != INVALID_RELTIME && rt <= t;
}

/*
 *		tintervallenge	- returns 1, iff length of interval i is greater or
 *								equal than reltime t
 */
bool
tintervallenge(TimeInterval i, RelativeTime t)
{
	RelativeTime rt;

	if ((i->status == T_INTERVAL_INVAL) || (t == INVALID_RELTIME))
		return 0;
	rt = tintervalrel(i);
	return rt != INVALID_RELTIME && rt >= t;
}

/*
 *		tintervalct		- returns 1, iff interval i1 contains interval i2
 */
bool
tintervalct(TimeInterval i1, TimeInterval i2)
{
	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		return 0;
	return (abstimele(i1->data[0], i2->data[0]) &&
			abstimege(i1->data[1], i2->data[1]));
}

/*
 *		tintervalov		- returns 1, iff interval i1 (partially) overlaps i2
 */
bool
tintervalov(TimeInterval i1, TimeInterval i2)
{
	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		return 0;
	return (!(abstimelt(i1->data[1], i2->data[0]) ||
			  abstimegt(i1->data[0], i2->data[1])));
}

/*
 *		tintervalstart	- returns  the start of interval i
 */
AbsoluteTime
tintervalstart(TimeInterval i)
{
	if (i->status == T_INTERVAL_INVAL)
		return INVALID_ABSTIME;
	return i->data[0];
}

/*
 *		tintervalend		- returns  the end of interval i
 */
AbsoluteTime
tintervalend(TimeInterval i)
{
	if (i->status == T_INTERVAL_INVAL)
		return INVALID_ABSTIME;
	return i->data[1];
}


/*****************************************************************************
 *	 PRIVATE ROUTINES														 *
 *****************************************************************************/

#ifdef NOT_USED
/*
 *		isreltime		- returns 1, iff datestring is of type reltime
 *								  2, iff datestring is 'invalid time' identifier
 *								  0, iff datestring contains a syntax error
 *		VALID time	less or equal +/-  `@ 68 years'
 *
 */
int
isreltime(char *str)
{
	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	int			dtype;
	char	   *field[MAXDATEFIELDS];
	int			nf,
				ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + 1];

	if (!PointerIsValid(str))
		return 0;

	if (strlen(str) > MAXDATELEN)
		return 0;

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
		|| (DecodeDateDelta(field, ftype, nf, &dtype, tm, &fsec) != 0))
		return 0;

	switch (dtype)
	{
		case (DTK_DELTA):
			return (abs(tm->tm_year) <= 68) ? 1 : 0;
			break;

		case (DTK_INVALID):
			return 2;
			break;

		default:
			return 0;
			break;
	}

	return 0;
}	/* isreltime() */

#endif

#ifdef NOT_USED
int
dummyfunc()
{
	char	   *p;
	char		c;
	int			i;
	char		unit[UNITMAXLEN];
	char		direction[DIRMAXLEN];
	int			localSign;
	int			localUnitNumber;
	long		localQuantity;

	if (!PointerIsValid(sign))
		sign = &localSign;

	if (!PointerIsValid(unitnr))
		unitnr = &localUnitNumber;

	if (!PointerIsValid(quantity))
		quantity = &localQuantity;

	unit[0] = '\0';
	direction[0] = '\0';
	p = timestring;
	/* skip leading blanks */
	while ((c = *p) != '\0')
	{
		if (c != ' ')
			break;
		p++;
	}

	/* Test whether 'invalid time' identifier or not */
	if (!strncmp(INVALID_RELTIME_STR, p, strlen(INVALID_RELTIME_STR) + 1))
		return 2;				/* correct 'invalid time' identifier found */

	/* handle label of relative time */
	if (c != RELTIME_LABEL)
		return 0;				/* syntax error */
	c = *++p;
	if (c != ' ')
		return 0;				/* syntax error */
	p++;
	/* handle the quantity */
	*quantity = 0;
	for (;;)
	{
		c = *p;
		if (isdigit(c))
		{
			*quantity = *quantity * 10 + (c - '0');
			p++;
		}
		else
		{
			if (c == ' ')
				break;			/* correct quantity found */
			else
				return 0;		/* syntax error */
		}
	}

	/* handle unit */
	p++;
	i = 0;
	for (;;)
	{
		c = *p;
		if (c >= 'a' && c <= 'z' && i <= (UNITMAXLEN - 1))
		{
			unit[i] = c;
			p++;
			i++;
		}
		else
		{
			if ((c == ' ' || c == '\0')
				&& correct_unit(unit, unitnr))
				break;			/* correct unit found */
			else
				return 0;		/* syntax error */
		}
	}

	/* handle optional direction */
	if (c == ' ')
		p++;
	i = 0;
	*sign = 1;
	for (;;)
	{
		c = *p;
		if (c >= 'a' && c <= 'z' && i <= (DIRMAXLEN - 1))
		{
			direction[i] = c;
			p++;
			i++;
		}
		else
		{
			if ((c == ' ' || c == '\0') && i == 0)
			{
				*sign = 1;
				break;			/* no direction specified */
			}
			if ((c == ' ' || c == '\0') && i != 0)
			{
				direction[i] = '\0';
				correct_dir(direction, sign);
				break;			/* correct direction found */
			}
			else
				return 0;		/* syntax error */
		}
	}

	return 1;
}

/*
 *		correct_unit	- returns 1, iff unit is a correct unit description
 *
 *		output parameter:
 *				unptr: points to an integer which is the appropriate unit number
 *					   (see function isreltime())
 */
static int
correct_unit(char *unit, int *unptr)
{
	int			j = 0;

	while (j < NUNITS)
	{
		if (strncmp(unit, unit_tab[j], strlen(unit_tab[j])) == 0)
		{
			*unptr = j;
			return 1;
		}
		j++;
	}
	return 0;					/* invalid unit descriptor */
}

/*
 *		correct_dir		- returns 1, iff direction is a correct identifier
 *
 *		output parameter:
 *				signptr: points to -1 if dir corresponds to past tense
 *						 else  to 1
 */
static int
correct_dir(char *direction, int *signptr)
{
	*signptr = 1;
	if (strncmp(RELTIME_PAST, direction, strlen(RELTIME_PAST) + 1) == 0)
	{
		*signptr = -1;
		return 1;
	}
	else
		return 0;				/* invalid direction descriptor */
}

#endif

/*
 *		istinterval		- returns 1, iff i_string is a valid interval descr.
 *								  0, iff i_string is NOT a valid interval desc.
 *								  2, iff any time is INVALID_ABSTIME
 *
 *		output parameter:
 *				i_start, i_end: interval margins
 *
 *		Time interval:
 *		`[' {` '} `'' <AbsTime> `'' {` '} `'' <AbsTime> `'' {` '} `]'
 *
 *		OR	`Undefined Range'	(see also INVALID_INTERVAL_STR)
 *
 *		where <AbsTime> satisfies the syntax of absolute time.
 *
 *		e.g.  [  '  Jan 18 1902'   'Jan 1 00:00:00 1970']
 */
static int
istinterval(char *i_string,
			AbsoluteTime *i_start,
			AbsoluteTime *i_end)
{
	char	   *p,
			   *p1;
	char		c;

	p = i_string;
	/* skip leading blanks up to '[' */
	while ((c = *p) != '\0')
	{
		if (IsSpace(c))
			p++;
		else if (c != '[')
			return 0;			/* syntax error */
		else
			break;
	}
	p++;
	/* skip leading blanks up to "'" */
	while ((c = *p) != '\0')
	{
		if (IsSpace(c))
			p++;
		else if (c != '"')
			return 0;			/* syntax error */
		else
			break;
	}
	p++;
	if (strncmp(INVALID_INTERVAL_STR, p, strlen(INVALID_INTERVAL_STR)) == 0)
		return 0;				/* undefined range, handled like a syntax
								 * err. */
	/* search for the end of the first date and change it to a NULL */
	p1 = p;
	while ((c = *p1) != '\0')
	{
		if (c == '"')
		{
			*p1 = '\0';
			break;
		}
		p1++;
	}
	/* get the first date */
	*i_start = nabstimein(p);	/* first absolute date */
	/* rechange NULL at the end of the first date to a "'" */
	*p1 = '"';
	p = ++p1;
	/* skip blanks up to "'", beginning of second date */
	while ((c = *p) != '\0')
	{
		if (IsSpace(c))
			p++;
		else if (c != '"')
			return 0;			/* syntax error */
		else
			break;
	}
	p++;
	/* search for the end of the second date and change it to a NULL */
	p1 = p;
	while ((c = *p1) != '\0')
	{
		if (c == '"')
		{
			*p1 = '\0';
			break;
		}
		p1++;
	}
	/* get the second date */
	*i_end = nabstimein(p);		/* second absolute date */
	/* rechange NULL at the end of the first date to a ''' */
	*p1 = '"';
	p = ++p1;
	/* skip blanks up to ']' */
	while ((c = *p) != '\0')
	{
		if (IsSpace(c))
			p++;
		else if (c != ']')
			return 0;			/* syntax error */
		else
			break;
	}
	p++;
	c = *p;
	if (c != '\0')
		return 0;				/* syntax error */
	/* it seems to be a valid interval */
	return 1;
}


/*****************************************************************************
 *
 *****************************************************************************/

int32							/* RelativeTime */
int4reltime(int32 timevalue)
{
	return timevalue;
}

/*
 * timeofday -
 *	   returns the current time as a text. similar to timenow() but returns
 *	   seconds with more precision (up to microsecs). (I need this to compare
 *	   the Wisconsin benchmark with Illustra whose TimeNow() shows current
 *	   time with precision up to microsecs.)			  - ay 3/95
 */
text *
timeofday(void)
{

	struct timeval tp;
	struct timezone tpz;
	char		templ[500];
	char		buf[500];
	text	   *tm;
	int			len = 0;

	gettimeofday(&tp, &tpz);
	strftime(templ, sizeof(templ), "%a %b %d %H:%M:%S.%%d %Y %Z",
			 localtime((time_t *) &tp.tv_sec));
	sprintf(buf, templ, tp.tv_usec);

	len = VARHDRSZ + strlen(buf);
	tm = (text *) palloc(len);
	VARSIZE(tm) = len;
	strncpy(VARDATA(tm), buf, strlen(buf));
	return tm;
}
