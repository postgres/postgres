/*-------------------------------------------------------------------------
 * nabstime.c
 *	  Utilities for the built-in type "AbsoluteTime".
 *	  Functions for the built-in type "RelativeTime".
 *	  Functions for the built-in type "TimeInterval".
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/nabstime.c,v 1.83 2001/03/22 03:59:52 momjian Exp $
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <float.h>
#include <limits.h>

#if !(defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE))
#include <sys/timeb.h>
#endif

#include "access/xact.h"
#include "miscadmin.h"
#include "utils/builtins.h"


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

#define ABSTIMEMIN(t1, t2) \
	(DatumGetBool(DirectFunctionCall2(abstimele, \
				  AbsoluteTimeGetDatum(t1), \
				  AbsoluteTimeGetDatum(t2))) ? (t1) : (t2))
#define ABSTIMEMAX(t1, t2) \
	(DatumGetBool(DirectFunctionCall2(abstimelt, \
				  AbsoluteTimeGetDatum(t1), \
				  AbsoluteTimeGetDatum(t2))) ? (t2) : (t1))

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

static AbsoluteTime tm2abstime(struct tm * tm, int tz);
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

#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
	struct tm  *tm;

	now = time(NULL);
#else
	struct timeb tb;			/* the old V7-ism */

	ftime(&tb);
	now = tb.time;
#endif

	if (!HasCTZSet)
	{
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
		CTimeZone = ((tm->tm_isdst > 0) ? (TIMEZONE_GLOBAL - 3600) : TIMEZONE_GLOBAL);
		strcpy(CTZName, tzname[tm->tm_isdst]);
#else							/* neither HAVE_TM_ZONE nor
								 * HAVE_INT_TIMEZONE */
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
abstime2tm(AbsoluteTime _time, int *tzp, struct tm * tm, char *tzn)
{
	time_t		time = (time_t) _time;

#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
	struct tm  *tx;

#else
	struct timeb tb;			/* the old V7-ism */

	ftime(&tb);
#endif


#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
	if (tzp != NULL)
	{
		tx = localtime((time_t *) &time);
#ifdef NO_MKTIME_BEFORE_1970
		if (tx->tm_year < 70 && tx->tm_isdst == 1)
		{
			time -= 3600;
			tx = localtime((time_t *) &time);
			tx->tm_isdst = 0;
		}
#endif
	}
	else
	{
		tx = gmtime((time_t *) &time);
	};

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
		StrNCpy(tzn, tm->tm_zone, MAXTZLEN + 1);
		if (strlen(tm->tm_zone) > MAXTZLEN)
			elog(NOTICE, "Invalid timezone \'%s\'", tm->tm_zone);
	}
#elif defined(HAVE_INT_TIMEZONE)
	if (tzp != NULL)
		*tzp = ((tm->tm_isdst > 0) ? (TIMEZONE_GLOBAL - 3600) : TIMEZONE_GLOBAL);

	if (tzn != NULL)
	{

		/*
		 * Copy no more than MAXTZLEN bytes of timezone to tzn, in case it
		 * contains an error message, which doesn't fit in the buffer
		 */
		StrNCpy(tzn, tzname[tm->tm_isdst], MAXTZLEN + 1);
		if (strlen(tzname[tm->tm_isdst]) > MAXTZLEN)
			elog(NOTICE, "Invalid timezone \'%s\'", tzname[tm->tm_isdst]);
	}
#endif
#else							/* not (HAVE_TM_ZONE || HAVE_INT_TIMEZONE) */
	if (tzp != NULL)
		*tzp = tb.timezone * 60;

	/*
	 * XXX does this work to get the local timezone string in V7? - tgl
	 * 97/03/18
	 */
	if (tzn != NULL)
	{
		strftime(tzn, MAXTZLEN, "%Z", localtime(&now));
		tzn[MAXTZLEN] = '\0';	/* let's just be sure it's null-terminated */
	}
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
	int			day;
	AbsoluteTime sec;

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
Datum
nabstimein(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
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

	PG_RETURN_ABSOLUTETIME(result);
}


/* nabstimeout()
 * Given an AbsoluteTime return the English text version of the date
 */
Datum
nabstimeout(PG_FUNCTION_ARGS)
{
	AbsoluteTime time = PG_GETARG_ABSOLUTETIME(0);
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

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}


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
Datum
abstime_finite(PG_FUNCTION_ARGS)
{
	AbsoluteTime abstime = PG_GETARG_ABSOLUTETIME(0);

	PG_RETURN_BOOL((abstime != INVALID_ABSTIME) &&
				   (abstime != NOSTART_ABSTIME) &&
				   (abstime != NOEND_ABSTIME));
}


/*
 *		abstimeeq		- returns true iff arguments are equal
 *		abstimene		- returns true iff arguments are not equal
 *		abstimelt		- returns true iff t1 less than t2
 *		abstimegt		- returns true iff t1 greater than t2
 *		abstimele		- returns true iff t1 less than or equal to t2
 *		abstimege		- returns true iff t1 greater than or equal to t2
 */
Datum
abstimeeq(PG_FUNCTION_ARGS)
{
	AbsoluteTime t1 = PG_GETARG_ABSOLUTETIME(0);
	AbsoluteTime t2 = PG_GETARG_ABSOLUTETIME(1);

	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		PG_RETURN_BOOL(false);
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	PG_RETURN_BOOL(t1 == t2);
}

Datum
abstimene(PG_FUNCTION_ARGS)
{
	AbsoluteTime t1 = PG_GETARG_ABSOLUTETIME(0);
	AbsoluteTime t2 = PG_GETARG_ABSOLUTETIME(1);

	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		PG_RETURN_BOOL(false);
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	PG_RETURN_BOOL(t1 != t2);
}

Datum
abstimelt(PG_FUNCTION_ARGS)
{
	AbsoluteTime t1 = PG_GETARG_ABSOLUTETIME(0);
	AbsoluteTime t2 = PG_GETARG_ABSOLUTETIME(1);

	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		PG_RETURN_BOOL(false);
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	PG_RETURN_BOOL(t1 < t2);
}

Datum
abstimegt(PG_FUNCTION_ARGS)
{
	AbsoluteTime t1 = PG_GETARG_ABSOLUTETIME(0);
	AbsoluteTime t2 = PG_GETARG_ABSOLUTETIME(1);

	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		PG_RETURN_BOOL(false);
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	PG_RETURN_BOOL(t1 > t2);
}

Datum
abstimele(PG_FUNCTION_ARGS)
{
	AbsoluteTime t1 = PG_GETARG_ABSOLUTETIME(0);
	AbsoluteTime t2 = PG_GETARG_ABSOLUTETIME(1);

	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		PG_RETURN_BOOL(false);
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	PG_RETURN_BOOL(t1 <= t2);
}

Datum
abstimege(PG_FUNCTION_ARGS)
{
	AbsoluteTime t1 = PG_GETARG_ABSOLUTETIME(0);
	AbsoluteTime t2 = PG_GETARG_ABSOLUTETIME(1);

	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		PG_RETURN_BOOL(false);
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	PG_RETURN_BOOL(t1 >= t2);
}


/* datetime_abstime()
 * Convert timestamp to abstime.
 */
Datum
timestamp_abstime(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	AbsoluteTime result;
	double		fsec;
	struct tm	tt,
			   *tm = &tt;

	if (TIMESTAMP_IS_INVALID(timestamp))
		result = INVALID_ABSTIME;
	else if (TIMESTAMP_IS_NOBEGIN(timestamp))
		result = NOSTART_ABSTIME;
	else if (TIMESTAMP_IS_NOEND(timestamp))
		result = NOEND_ABSTIME;
	else
	{
		if (TIMESTAMP_IS_RELATIVE(timestamp))
		{
			timestamp2tm(SetTimestamp(timestamp), NULL, tm, &fsec, NULL);
			result = tm2abstime(tm, 0);
		}
		else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL) == 0)
			result = tm2abstime(tm, 0);
		else
			result = INVALID_ABSTIME;
	}

	PG_RETURN_ABSOLUTETIME(result);
}

/* abstime_timestamp()
 * Convert abstime to timestamp.
 */
Datum
abstime_timestamp(PG_FUNCTION_ARGS)
{
	AbsoluteTime abstime = PG_GETARG_ABSOLUTETIME(0);
	Timestamp	result;

	switch (abstime)
	{
		case INVALID_ABSTIME:
			TIMESTAMP_INVALID(result);
			break;

		case NOSTART_ABSTIME:
			TIMESTAMP_NOBEGIN(result);
			break;

		case NOEND_ABSTIME:
			TIMESTAMP_NOEND(result);
			break;

		case EPOCH_ABSTIME:
			TIMESTAMP_EPOCH(result);
			break;

		case CURRENT_ABSTIME:
			TIMESTAMP_CURRENT(result);
			break;

		default:
			result = abstime + ((date2j(1970, 1, 1) - date2j(2000, 1, 1)) * 86400);
			break;
	};

	PG_RETURN_TIMESTAMP(result);
}


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		reltimein		- converts a reltime string in an internal format
 */
Datum
reltimein(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	RelativeTime result;
	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	int			dtype;
	char	   *field[MAXDATEFIELDS];
	int			nf,
				ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + 1];

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
			PG_RETURN_RELATIVETIME(result);

		default:
			PG_RETURN_RELATIVETIME(INVALID_RELTIME);
	}

	elog(ERROR, "Bad reltime (internal coding error) '%s'", str);
	PG_RETURN_RELATIVETIME(INVALID_RELTIME);
}


/*
 *		reltimeout		- converts the internal format to a reltime string
 */
Datum
reltimeout(PG_FUNCTION_ARGS)
{
	RelativeTime time = PG_GETARG_RELATIVETIME(0);
	char	   *result;
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];

	if (time == INVALID_RELTIME)
		strcpy(buf, INVALID_RELTIME_STR);
	else
	{
		reltime2tm(time, tm);
		EncodeTimeSpan(tm, 0, DateStyle, buf);
	}

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}


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
 *		tintervalin		- converts an interval string to internal format
 */
Datum
tintervalin(PG_FUNCTION_ARGS)
{
	char	   *intervalstr = PG_GETARG_CSTRING(0);
	TimeInterval interval;
	int			error;
	AbsoluteTime i_start,
				i_end,
				t1,
				t2;

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
	PG_RETURN_TIMEINTERVAL(interval);
}


/*
 *		tintervalout	- converts an internal interval format to a string
 *
 */
Datum
tintervalout(PG_FUNCTION_ARGS)
{
	TimeInterval interval = PG_GETARG_TIMEINTERVAL(0);
	char	   *i_str,
			   *p;

	i_str = (char *) palloc(T_INTERVAL_LEN);	/* ['...' '...'] */
	strcpy(i_str, "[\"");
	if (interval->status == T_INTERVAL_INVAL)
		strcat(i_str, INVALID_INTERVAL_STR);
	else
	{
		p = DatumGetCString(DirectFunctionCall1(nabstimeout,
							   AbsoluteTimeGetDatum(interval->data[0])));
		strcat(i_str, p);
		pfree(p);
		strcat(i_str, "\" \"");
		p = DatumGetCString(DirectFunctionCall1(nabstimeout,
							   AbsoluteTimeGetDatum(interval->data[1])));
		strcat(i_str, p);
		pfree(p);
	}
	strcat(i_str, "\"]\0");
	PG_RETURN_CSTRING(i_str);
}


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

Datum
interval_reltime(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	RelativeTime time;
	int			year,
				month;
	double		span;

	if (INTERVAL_IS_INVALID(*interval))
		time = INVALID_RELTIME;
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

	PG_RETURN_RELATIVETIME(time);
}


Datum
reltime_interval(PG_FUNCTION_ARGS)
{
	RelativeTime reltime = PG_GETARG_RELATIVETIME(0);
	Interval   *result;
	int			year,
				month;

	result = (Interval *) palloc(sizeof(Interval));

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
			break;
	}

	PG_RETURN_INTERVAL_P(result);
}


/*
 *		mktinterval		- creates a time interval with endpoints t1 and t2
 */
Datum
mktinterval(PG_FUNCTION_ARGS)
{
	AbsoluteTime t1 = PG_GETARG_ABSOLUTETIME(0);
	AbsoluteTime t2 = PG_GETARG_ABSOLUTETIME(1);
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

	PG_RETURN_TIMEINTERVAL(interval);
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
Datum
timepl(PG_FUNCTION_ARGS)
{
	AbsoluteTime t1 = PG_GETARG_ABSOLUTETIME(0);
	RelativeTime t2 = PG_GETARG_RELATIVETIME(1);

	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();

	if (AbsoluteTimeIsReal(t1) &&
		RelativeTimeIsValid(t2) &&
		((t2 > 0) ? (t1 < NOEND_ABSTIME - t2)
		 : (t1 > NOSTART_ABSTIME - t2)))		/* prevent overflow */
		PG_RETURN_ABSOLUTETIME(t1 + t2);

	PG_RETURN_ABSOLUTETIME(INVALID_ABSTIME);
}


/*
 *		timemi			- returns the value of (abstime t1 - reltime t2)
 */
Datum
timemi(PG_FUNCTION_ARGS)
{
	AbsoluteTime t1 = PG_GETARG_ABSOLUTETIME(0);
	RelativeTime t2 = PG_GETARG_RELATIVETIME(1);

	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();

	if (AbsoluteTimeIsReal(t1) &&
		RelativeTimeIsValid(t2) &&
		((t2 > 0) ? (t1 > NOSTART_ABSTIME + t2)
		 : (t1 < NOEND_ABSTIME + t2)))	/* prevent overflow */
		PG_RETURN_ABSOLUTETIME(t1 - t2);

	PG_RETURN_ABSOLUTETIME(INVALID_ABSTIME);
}


/*
 *		abstimemi		- returns the value of (abstime t1 - abstime t2)
 *
 * This is not exported, so it's not been made fmgr-compatible.
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
 *		intinterval		- returns true iff absolute date is in the interval
 */
Datum
intinterval(PG_FUNCTION_ARGS)
{
	AbsoluteTime t = PG_GETARG_ABSOLUTETIME(0);
	TimeInterval interval = PG_GETARG_TIMEINTERVAL(1);

	if (interval->status == T_INTERVAL_VALID && t != INVALID_ABSTIME)
	{
		if (DatumGetBool(DirectFunctionCall2(abstimege,
											 AbsoluteTimeGetDatum(t),
							 AbsoluteTimeGetDatum(interval->data[0]))) &&
			DatumGetBool(DirectFunctionCall2(abstimele,
											 AbsoluteTimeGetDatum(t),
							   AbsoluteTimeGetDatum(interval->data[1]))))
			PG_RETURN_BOOL(true);
	}
	PG_RETURN_BOOL(false);
}

/*
 *		tintervalrel		- returns  relative time corresponding to interval
 */
Datum
tintervalrel(PG_FUNCTION_ARGS)
{
	TimeInterval interval = PG_GETARG_TIMEINTERVAL(0);

	if (interval->status != T_INTERVAL_VALID)
		PG_RETURN_RELATIVETIME(INVALID_RELTIME);

	PG_RETURN_RELATIVETIME(abstimemi(interval->data[1], interval->data[0]));
}

/*
 *		timenow			- returns  time "now", internal format
 *
 *		Now AbsoluteTime is time since Jan 1 1970 -mer 7 Feb 1992
 */
Datum
timenow(PG_FUNCTION_ARGS)
{
	time_t		sec;

	if (time(&sec) < 0)
		PG_RETURN_ABSOLUTETIME(INVALID_ABSTIME);
	PG_RETURN_ABSOLUTETIME((AbsoluteTime) sec);
}

/*
 *		reltimeeq		- returns true iff arguments are equal
 *		reltimene		- returns true iff arguments are not equal
 *		reltimelt		- returns true iff t1 less than t2
 *		reltimegt		- returns true iff t1 greater than t2
 *		reltimele		- returns true iff t1 less than or equal to t2
 *		reltimege		- returns true iff t1 greater than or equal to t2
 */
Datum
reltimeeq(PG_FUNCTION_ARGS)
{
	RelativeTime t1 = PG_GETARG_RELATIVETIME(0);
	RelativeTime t2 = PG_GETARG_RELATIVETIME(1);

	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(t1 == t2);
}

Datum
reltimene(PG_FUNCTION_ARGS)
{
	RelativeTime t1 = PG_GETARG_RELATIVETIME(0);
	RelativeTime t2 = PG_GETARG_RELATIVETIME(1);

	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(t1 != t2);
}

Datum
reltimelt(PG_FUNCTION_ARGS)
{
	RelativeTime t1 = PG_GETARG_RELATIVETIME(0);
	RelativeTime t2 = PG_GETARG_RELATIVETIME(1);

	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(t1 < t2);
}

Datum
reltimegt(PG_FUNCTION_ARGS)
{
	RelativeTime t1 = PG_GETARG_RELATIVETIME(0);
	RelativeTime t2 = PG_GETARG_RELATIVETIME(1);

	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(t1 > t2);
}

Datum
reltimele(PG_FUNCTION_ARGS)
{
	RelativeTime t1 = PG_GETARG_RELATIVETIME(0);
	RelativeTime t2 = PG_GETARG_RELATIVETIME(1);

	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(t1 <= t2);
}

Datum
reltimege(PG_FUNCTION_ARGS)
{
	RelativeTime t1 = PG_GETARG_RELATIVETIME(0);
	RelativeTime t2 = PG_GETARG_RELATIVETIME(1);

	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(t1 >= t2);
}


/*
 *		tintervalsame	- returns true iff interval i1 is same as interval i2
 *		Check begin and end time.
 */
Datum
tintervalsame(PG_FUNCTION_ARGS)
{
	TimeInterval i1 = PG_GETARG_TIMEINTERVAL(0);
	TimeInterval i2 = PG_GETARG_TIMEINTERVAL(1);

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		PG_RETURN_BOOL(false);

	if (DatumGetBool(DirectFunctionCall2(abstimeeq,
									   AbsoluteTimeGetDatum(i1->data[0]),
								   AbsoluteTimeGetDatum(i2->data[0]))) &&
		DatumGetBool(DirectFunctionCall2(abstimeeq,
									   AbsoluteTimeGetDatum(i1->data[1]),
									 AbsoluteTimeGetDatum(i2->data[1]))))
		PG_RETURN_BOOL(true);
	PG_RETURN_BOOL(false);
}


/*
 *		tintervaleq		- returns true iff interval i1 is equal to interval i2
 *		Check length of intervals.
 */
Datum
tintervaleq(PG_FUNCTION_ARGS)
{
	TimeInterval i1 = PG_GETARG_TIMEINTERVAL(0);
	TimeInterval i2 = PG_GETARG_TIMEINTERVAL(1);
	AbsoluteTime t10,
				t11,
				t20,
				t21;

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		PG_RETURN_BOOL(false);

	t10 = i1->data[0];
	t11 = i1->data[1];
	t20 = i2->data[0];
	t21 = i2->data[1];

	if ((t10 == INVALID_ABSTIME) || (t20 == INVALID_ABSTIME)
		|| (t20 == INVALID_ABSTIME) || (t21 == INVALID_ABSTIME))
		PG_RETURN_BOOL(false);

	if (t10 == CURRENT_ABSTIME)
		t10 = GetCurrentTransactionStartTime();
	if (t11 == CURRENT_ABSTIME)
		t11 = GetCurrentTransactionStartTime();
	if (t20 == CURRENT_ABSTIME)
		t20 = GetCurrentTransactionStartTime();
	if (t21 == CURRENT_ABSTIME)
		t21 = GetCurrentTransactionStartTime();

	PG_RETURN_BOOL((t11 - t10) == (t21 - t20));
}

Datum
tintervalne(PG_FUNCTION_ARGS)
{
	TimeInterval i1 = PG_GETARG_TIMEINTERVAL(0);
	TimeInterval i2 = PG_GETARG_TIMEINTERVAL(1);
	AbsoluteTime t10,
				t11,
				t20,
				t21;

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		PG_RETURN_BOOL(false);

	t10 = i1->data[0];
	t11 = i1->data[1];
	t20 = i2->data[0];
	t21 = i2->data[1];

	if ((t10 == INVALID_ABSTIME) || (t20 == INVALID_ABSTIME)
		|| (t20 == INVALID_ABSTIME) || (t21 == INVALID_ABSTIME))
		PG_RETURN_BOOL(false);

	if (t10 == CURRENT_ABSTIME)
		t10 = GetCurrentTransactionStartTime();
	if (t11 == CURRENT_ABSTIME)
		t11 = GetCurrentTransactionStartTime();
	if (t20 == CURRENT_ABSTIME)
		t20 = GetCurrentTransactionStartTime();
	if (t21 == CURRENT_ABSTIME)
		t21 = GetCurrentTransactionStartTime();

	PG_RETURN_BOOL((t11 - t10) != (t21 - t20));
}

Datum
tintervallt(PG_FUNCTION_ARGS)
{
	TimeInterval i1 = PG_GETARG_TIMEINTERVAL(0);
	TimeInterval i2 = PG_GETARG_TIMEINTERVAL(1);
	AbsoluteTime t10,
				t11,
				t20,
				t21;

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		PG_RETURN_BOOL(false);

	t10 = i1->data[0];
	t11 = i1->data[1];
	t20 = i2->data[0];
	t21 = i2->data[1];

	if ((t10 == INVALID_ABSTIME) || (t20 == INVALID_ABSTIME)
		|| (t20 == INVALID_ABSTIME) || (t21 == INVALID_ABSTIME))
		PG_RETURN_BOOL(false);

	if (t10 == CURRENT_ABSTIME)
		t10 = GetCurrentTransactionStartTime();
	if (t11 == CURRENT_ABSTIME)
		t11 = GetCurrentTransactionStartTime();
	if (t20 == CURRENT_ABSTIME)
		t20 = GetCurrentTransactionStartTime();
	if (t21 == CURRENT_ABSTIME)
		t21 = GetCurrentTransactionStartTime();

	PG_RETURN_BOOL((t11 - t10) < (t21 - t20));
}

Datum
tintervalle(PG_FUNCTION_ARGS)
{
	TimeInterval i1 = PG_GETARG_TIMEINTERVAL(0);
	TimeInterval i2 = PG_GETARG_TIMEINTERVAL(1);
	AbsoluteTime t10,
				t11,
				t20,
				t21;

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		PG_RETURN_BOOL(false);

	t10 = i1->data[0];
	t11 = i1->data[1];
	t20 = i2->data[0];
	t21 = i2->data[1];

	if ((t10 == INVALID_ABSTIME) || (t20 == INVALID_ABSTIME)
		|| (t20 == INVALID_ABSTIME) || (t21 == INVALID_ABSTIME))
		PG_RETURN_BOOL(false);

	if (t10 == CURRENT_ABSTIME)
		t10 = GetCurrentTransactionStartTime();
	if (t11 == CURRENT_ABSTIME)
		t11 = GetCurrentTransactionStartTime();
	if (t20 == CURRENT_ABSTIME)
		t20 = GetCurrentTransactionStartTime();
	if (t21 == CURRENT_ABSTIME)
		t21 = GetCurrentTransactionStartTime();

	PG_RETURN_BOOL((t11 - t10) <= (t21 - t20));
}

Datum
tintervalgt(PG_FUNCTION_ARGS)
{
	TimeInterval i1 = PG_GETARG_TIMEINTERVAL(0);
	TimeInterval i2 = PG_GETARG_TIMEINTERVAL(1);
	AbsoluteTime t10,
				t11,
				t20,
				t21;

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		PG_RETURN_BOOL(false);

	t10 = i1->data[0];
	t11 = i1->data[1];
	t20 = i2->data[0];
	t21 = i2->data[1];

	if ((t10 == INVALID_ABSTIME) || (t20 == INVALID_ABSTIME)
		|| (t20 == INVALID_ABSTIME) || (t21 == INVALID_ABSTIME))
		PG_RETURN_BOOL(false);

	if (t10 == CURRENT_ABSTIME)
		t10 = GetCurrentTransactionStartTime();
	if (t11 == CURRENT_ABSTIME)
		t11 = GetCurrentTransactionStartTime();
	if (t20 == CURRENT_ABSTIME)
		t20 = GetCurrentTransactionStartTime();
	if (t21 == CURRENT_ABSTIME)
		t21 = GetCurrentTransactionStartTime();

	PG_RETURN_BOOL((t11 - t10) > (t21 - t20));
}

Datum
tintervalge(PG_FUNCTION_ARGS)
{
	TimeInterval i1 = PG_GETARG_TIMEINTERVAL(0);
	TimeInterval i2 = PG_GETARG_TIMEINTERVAL(1);
	AbsoluteTime t10,
				t11,
				t20,
				t21;

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		PG_RETURN_BOOL(false);

	t10 = i1->data[0];
	t11 = i1->data[1];
	t20 = i2->data[0];
	t21 = i2->data[1];

	if ((t10 == INVALID_ABSTIME) || (t20 == INVALID_ABSTIME)
		|| (t20 == INVALID_ABSTIME) || (t21 == INVALID_ABSTIME))
		PG_RETURN_BOOL(false);

	if (t10 == CURRENT_ABSTIME)
		t10 = GetCurrentTransactionStartTime();
	if (t11 == CURRENT_ABSTIME)
		t11 = GetCurrentTransactionStartTime();
	if (t20 == CURRENT_ABSTIME)
		t20 = GetCurrentTransactionStartTime();
	if (t21 == CURRENT_ABSTIME)
		t21 = GetCurrentTransactionStartTime();

	PG_RETURN_BOOL((t11 - t10) >= (t21 - t20));
}


/*
 *		tintervalleneq	- returns true iff length of interval i is equal to
 *								reltime t
 *		tintervallenne	- returns true iff length of interval i is not equal
 *								to reltime t
 *		tintervallenlt	- returns true iff length of interval i is less than
 *								reltime t
 *		tintervallengt	- returns true iff length of interval i is greater
 *								than reltime t
 *		tintervallenle	- returns true iff length of interval i is less or
 *								equal than reltime t
 *		tintervallenge	- returns true iff length of interval i is greater or
 *								equal than reltime t
 */
Datum
tintervalleneq(PG_FUNCTION_ARGS)
{
	TimeInterval i = PG_GETARG_TIMEINTERVAL(0);
	RelativeTime t = PG_GETARG_RELATIVETIME(1);
	RelativeTime rt;

	if (i->status == T_INTERVAL_INVAL || t == INVALID_RELTIME)
		PG_RETURN_BOOL(false);
	rt = DatumGetRelativeTime(DirectFunctionCall1(tintervalrel,
											   TimeIntervalGetDatum(i)));
	PG_RETURN_BOOL(rt != INVALID_RELTIME && rt == t);
}

Datum
tintervallenne(PG_FUNCTION_ARGS)
{
	TimeInterval i = PG_GETARG_TIMEINTERVAL(0);
	RelativeTime t = PG_GETARG_RELATIVETIME(1);
	RelativeTime rt;

	if (i->status == T_INTERVAL_INVAL || t == INVALID_RELTIME)
		PG_RETURN_BOOL(false);
	rt = DatumGetRelativeTime(DirectFunctionCall1(tintervalrel,
											   TimeIntervalGetDatum(i)));
	PG_RETURN_BOOL(rt != INVALID_RELTIME && rt != t);
}

Datum
tintervallenlt(PG_FUNCTION_ARGS)
{
	TimeInterval i = PG_GETARG_TIMEINTERVAL(0);
	RelativeTime t = PG_GETARG_RELATIVETIME(1);
	RelativeTime rt;

	if (i->status == T_INTERVAL_INVAL || t == INVALID_RELTIME)
		PG_RETURN_BOOL(false);
	rt = DatumGetRelativeTime(DirectFunctionCall1(tintervalrel,
											   TimeIntervalGetDatum(i)));
	PG_RETURN_BOOL(rt != INVALID_RELTIME && rt < t);
}

Datum
tintervallengt(PG_FUNCTION_ARGS)
{
	TimeInterval i = PG_GETARG_TIMEINTERVAL(0);
	RelativeTime t = PG_GETARG_RELATIVETIME(1);
	RelativeTime rt;

	if (i->status == T_INTERVAL_INVAL || t == INVALID_RELTIME)
		PG_RETURN_BOOL(false);
	rt = DatumGetRelativeTime(DirectFunctionCall1(tintervalrel,
											   TimeIntervalGetDatum(i)));
	PG_RETURN_BOOL(rt != INVALID_RELTIME && rt > t);
}

Datum
tintervallenle(PG_FUNCTION_ARGS)
{
	TimeInterval i = PG_GETARG_TIMEINTERVAL(0);
	RelativeTime t = PG_GETARG_RELATIVETIME(1);
	RelativeTime rt;

	if (i->status == T_INTERVAL_INVAL || t == INVALID_RELTIME)
		PG_RETURN_BOOL(false);
	rt = DatumGetRelativeTime(DirectFunctionCall1(tintervalrel,
											   TimeIntervalGetDatum(i)));
	PG_RETURN_BOOL(rt != INVALID_RELTIME && rt <= t);
}

Datum
tintervallenge(PG_FUNCTION_ARGS)
{
	TimeInterval i = PG_GETARG_TIMEINTERVAL(0);
	RelativeTime t = PG_GETARG_RELATIVETIME(1);
	RelativeTime rt;

	if (i->status == T_INTERVAL_INVAL || t == INVALID_RELTIME)
		PG_RETURN_BOOL(false);
	rt = DatumGetRelativeTime(DirectFunctionCall1(tintervalrel,
											   TimeIntervalGetDatum(i)));
	PG_RETURN_BOOL(rt != INVALID_RELTIME && rt >= t);
}

/*
 *		tintervalct		- returns true iff interval i1 contains interval i2
 */
Datum
tintervalct(PG_FUNCTION_ARGS)
{
	TimeInterval i1 = PG_GETARG_TIMEINTERVAL(0);
	TimeInterval i2 = PG_GETARG_TIMEINTERVAL(1);

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		PG_RETURN_BOOL(false);
	if (DatumGetBool(DirectFunctionCall2(abstimele,
									   AbsoluteTimeGetDatum(i1->data[0]),
								   AbsoluteTimeGetDatum(i2->data[0]))) &&
		DatumGetBool(DirectFunctionCall2(abstimege,
									   AbsoluteTimeGetDatum(i1->data[1]),
									 AbsoluteTimeGetDatum(i2->data[1]))))
		PG_RETURN_BOOL(true);
	PG_RETURN_BOOL(false);
}

/*
 *		tintervalov		- returns true iff interval i1 (partially) overlaps i2
 */
Datum
tintervalov(PG_FUNCTION_ARGS)
{
	TimeInterval i1 = PG_GETARG_TIMEINTERVAL(0);
	TimeInterval i2 = PG_GETARG_TIMEINTERVAL(1);

	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		PG_RETURN_BOOL(false);
	if (DatumGetBool(DirectFunctionCall2(abstimelt,
									   AbsoluteTimeGetDatum(i1->data[1]),
								   AbsoluteTimeGetDatum(i2->data[0]))) ||
		DatumGetBool(DirectFunctionCall2(abstimegt,
									   AbsoluteTimeGetDatum(i1->data[0]),
									 AbsoluteTimeGetDatum(i2->data[1]))))
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(true);
}

/*
 *		tintervalstart	- returns  the start of interval i
 */
Datum
tintervalstart(PG_FUNCTION_ARGS)
{
	TimeInterval i = PG_GETARG_TIMEINTERVAL(0);

	if (i->status == T_INTERVAL_INVAL)
		PG_RETURN_ABSOLUTETIME(INVALID_ABSTIME);
	PG_RETURN_ABSOLUTETIME(i->data[0]);
}

/*
 *		tintervalend		- returns  the end of interval i
 */
Datum
tintervalend(PG_FUNCTION_ARGS)
{
	TimeInterval i = PG_GETARG_TIMEINTERVAL(0);

	if (i->status == T_INTERVAL_INVAL)
		PG_RETURN_ABSOLUTETIME(INVALID_ABSTIME);
	PG_RETURN_ABSOLUTETIME(i->data[1]);
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
		if (isdigit((unsigned char) c))
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
	*i_start = DatumGetAbsoluteTime(DirectFunctionCall1(nabstimein,
													CStringGetDatum(p)));
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
	*i_end = DatumGetAbsoluteTime(DirectFunctionCall1(nabstimein,
													CStringGetDatum(p)));
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

Datum
int4reltime(PG_FUNCTION_ARGS)
{
	int32		timevalue = PG_GETARG_INT32(0);

	/* Just coerce it directly to RelativeTime ... */
	PG_RETURN_RELATIVETIME((RelativeTime) timevalue);
}

/*
 * timeofday -
 *	   returns the current time as a text. similar to timenow() but returns
 *	   seconds with more precision (up to microsecs). (I need this to compare
 *	   the Wisconsin benchmark with Illustra whose TimeNow() shows current
 *	   time with precision up to microsecs.)			  - ay 3/95
 */
Datum
timeofday(PG_FUNCTION_ARGS)
{
	struct timeval tp;
	struct timezone tpz;
	char		templ[100];
	char		buf[100];
	text	   *result;
	int			len;

	gettimeofday(&tp, &tpz);
	strftime(templ, sizeof(templ), "%a %b %d %H:%M:%S.%%06d %Y %Z",
			 localtime((time_t *) &tp.tv_sec));
	snprintf(buf, sizeof(buf), templ, tp.tv_usec);

	len = VARHDRSZ + strlen(buf);
	result = (text *) palloc(len);
	VARATT_SIZEP(result) = len;
	memcpy(VARDATA(result), buf, strlen(buf));
	PG_RETURN_TEXT_P(result);
}
