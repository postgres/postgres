/*
 * nabstime.c--
 *	  parse almost any absolute date getdate(3) can (& some it can't)
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *	  $Id: nabstime.c,v 1.50 1998/12/15 15:28:57 scrappy Exp $
 *
 */
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>

#include "postgres.h"
#include <miscadmin.h>
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
#include "access/xact.h"

static AbsoluteTime tm2abstime(struct tm * tm, int tz);

#define MIN_DAYNUM -24856		/* December 13, 1901 */
#define MAX_DAYNUM 24854		/* January 18, 2038 */


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
#ifdef HAVE_TM_ZONE
		tm = localtime(&now);

		CTimeZone = -tm->tm_gmtoff;		/* tm_gmtoff is Sun/DEC-ism */
		CDayLight = (tm->tm_isdst > 0);

#if 0
		/*
		 * XXX is there a better way to get local timezone string w/o
		 * tzname? - tgl 97/03/18
		 */
		strftime(CTZName, MAXTZLEN, "%Z", tm);
#endif
		/* XXX FreeBSD man pages indicate that this should work - thomas 1998-12-12 */
		strcpy(CTZName, tm->tm_zone);

#elif defined(HAVE_INT_TIMEZONE)
		tm = localtime(&now);

		CDayLight = tm->tm_isdst;
		CTimeZone = (tm->tm_isdst ? (timezone - 3600) : timezone);
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

#ifdef DATEDEBUG
	printf("GetCurrentAbsoluteTime- timezone is %s -> %d seconds from UTC\n",
		   CTZName, CTimeZone);
#endif

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

#if defined(DATEDEBUG)
#if (! defined(HAVE_TM_ZONE)) && defined(HAVE_INT_TIMEZONE)
	printf("datetime2tm- (localtime) %d.%02d.%02d %02d:%02d:%02d %s %s dst=%d\n",
		   tx->tm_year, tx->tm_mon, tx->tm_mday, tx->tm_hour, tx->tm_min, tx->tm_sec,
		   tzname[0], tzname[1], tx->tm_isdst);
#else
	printf("datetime2tm- (localtime) %d.%02d.%02d %02d:%02d:%02d %s dst=%d\n",
		   tx->tm_year, tx->tm_mon, tx->tm_mday, tx->tm_hour, tx->tm_min, tx->tm_sec,
		   tx->tm_zone, tx->tm_isdst);
#endif
#endif

#ifdef USE_POSIX_TIME

	tm->tm_year = tx->tm_year + 1900;
	tm->tm_mon = tx->tm_mon + 1;
	tm->tm_mday = tx->tm_mday;
	tm->tm_hour = tx->tm_hour;
	tm->tm_min = tx->tm_min;
	tm->tm_sec = tx->tm_sec;
	tm->tm_isdst = tx->tm_isdst;

#ifdef HAVE_TM_ZONE
	tm->tm_gmtoff = tx->tm_gmtoff;
	tm->tm_zone = tx->tm_zone;

	if (tzp != NULL)
		*tzp = -tm->tm_gmtoff;	/* tm_gmtoff is Sun/DEC-ism */
	/* XXX FreeBSD man pages indicate that this should work - tgl 97/04/23 */
	if (tzn != NULL)
		strcpy(tzn, tm->tm_zone);
#elif defined(HAVE_INT_TIMEZONE)
	if (tzp != NULL)
		*tzp = (tm->tm_isdst ? (timezone - 3600) : timezone);
	if (tzn != NULL)
		strcpy(tzn, tzname[tm->tm_isdst]);
#else							/* !HAVE_INT_TIMEZONE */
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
		elog(ERROR, "Bad (null) abstime external representation", NULL);

	if (strlen(str) > MAXDATELEN)
		elog(ERROR, "Bad (length) abstime external representation '%s'", str);

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
	  || (DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz) != 0))
		elog(ERROR, "Bad abstime external representation '%s'", str);

#ifdef DATEDEBUG
	printf("nabstimein- %d fields are type %d (DTK_DATE=%d)\n", nf, dtype, DTK_DATE);
#endif

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
#if DATEDEBUG
#endif
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
 * Convert datetime to abstime.
 */
AbsoluteTime
datetime_abstime(DateTime *datetime)
{
	AbsoluteTime result;

	double		fsec;
	struct tm	tt,
			   *tm = &tt;

	if (!PointerIsValid(datetime))
	{
		result = INVALID_ABSTIME;

	}
	else if (DATETIME_IS_INVALID(*datetime))
	{
		result = INVALID_ABSTIME;

	}
	else if (DATETIME_IS_NOBEGIN(*datetime))
	{
		result = NOSTART_ABSTIME;

	}
	else if (DATETIME_IS_NOEND(*datetime))
	{
		result = NOEND_ABSTIME;

	}
	else
	{
		if (DATETIME_IS_RELATIVE(*datetime))
		{
			datetime2tm(SetDateTime(*datetime), NULL, tm, &fsec, NULL);
			result = tm2abstime(tm, 0);

		}
		else if (datetime2tm(*datetime, NULL, tm, &fsec, NULL) == 0)
		{
			result = tm2abstime(tm, 0);

		}
		else
		{
			result = INVALID_ABSTIME;
		};
	};

	return result;
}	/* datetime_abstime() */

/* abstime_datetime()
 * Convert abstime to datetime.
 */
DateTime   *
abstime_datetime(AbsoluteTime abstime)
{
	DateTime   *result;

	if (!PointerIsValid(result = palloc(sizeof(DateTime))))
		elog(ERROR, "Unable to allocate space to convert abstime to datetime", NULL);

	switch (abstime)
	{
		case INVALID_ABSTIME:
			DATETIME_INVALID(*result);
			break;

		case NOSTART_ABSTIME:
			DATETIME_NOBEGIN(*result);
			break;

		case NOEND_ABSTIME:
			DATETIME_NOEND(*result);
			break;

		case EPOCH_ABSTIME:
			DATETIME_EPOCH(*result);
			break;

		case CURRENT_ABSTIME:
			DATETIME_CURRENT(*result);
			break;

		default:
			*result = abstime + ((date2j(1970, 1, 1) - date2j(2000, 1, 1)) * 86400);
			break;
	};

	return result;
}	/* abstime_datetime() */
