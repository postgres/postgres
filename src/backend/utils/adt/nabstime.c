/*-------------------------------------------------------------------------
 *
 * nabstime.c--
 *    parse almost any absolute date getdate(3) can (& some it can't)
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/nabstime.c,v 1.20 1997/03/28 06:54:51 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>

#include "postgres.h"
#include <miscadmin.h>
#ifdef HAVE_FLOAT_H
# include <float.h>
#endif
#ifdef HAVE_LIMITS_H
# include <limits.h>
#endif
#ifndef USE_POSIX_TIME
#include <sys/timeb.h>
#endif
#include "access/xact.h"


#define MIN_DAYNUM -24856			/* December 13, 1901 */
#define MAX_DAYNUM 24854			/* January 18, 2038 */


/* GetCurrentAbsoluteTime()
 * Get the current system time. Set timezone parameters if not specified elsewhere.
 * Define HasTZSet to allow clients to specify the default timezone.
 *
 * Returns the number of seconds since epoch (January 1 1970 GMT)
 */

AbsoluteTime
GetCurrentAbsoluteTime(void)
{
    time_t now;

#ifdef USE_POSIX_TIME
    now = time(NULL);
#else /* ! USE_POSIX_TIME */
    struct timeb tbnow;		/* the old V7-ism */

    (void) ftime(&tbnow);
    now = tbnow.time;
#endif

    if (! HasCTZSet) {
#ifdef USE_POSIX_TIME
#if defined(HAVE_TZSET) && defined(HAVE_INT_TIMEZONE)
	tzset();
	CTimeZone = timezone;
	CDayLight = daylight;
	strcpy( CTZName, tzname[0]);
#else /* !HAVE_TZSET */
	struct tm *tmnow = localtime(&now);

	CTimeZone = - tmnow->tm_gmtoff;	/* tm_gmtoff is Sun/DEC-ism */
	CDayLight = (tmnow->tm_isdst > 0);
	/* XXX is there a better way to get local timezone string in V7? - tgl 97/03/18 */
	strftime( CTZName, MAXTZLEN, "%Z", localtime(&now));
#endif
#else /* ! USE_POSIX_TIME */
	CTimeZone = tbnow.timezone * 60;
	CDayLight = (tbnow.dstflag != 0);
	/* XXX does this work to get the local timezone string in V7? - tgl 97/03/18 */
	strftime( CTZName, MAXTZLEN, "%Z", localtime(&now));
#endif 
    };

#ifdef DATEDEBUG
printf( "GetCurrentAbsoluteTime- timezone is %s -> %d seconds from UTC\n",
 CTZName, CTimeZone);
#endif

    return((AbsoluteTime) now);
} /* GetCurrentAbsoluteTime() */


void
GetCurrentTime(struct tm *tm)
{
    time_t now;
    struct tm *tt;

    now = GetCurrentTransactionStartTime();
    tt = gmtime( &now);

    tm->tm_year = tt->tm_year+1900;
    tm->tm_mon = tt->tm_mon+1;
    tm->tm_mday = tt->tm_mday;
    tm->tm_hour = tt->tm_hour;
    tm->tm_min = tt->tm_min;
    tm->tm_sec = tt->tm_sec;
    tm->tm_isdst = tt->tm_isdst;

    return;
} /* GetCurrentTime() */


AbsoluteTime tm2abstime( struct tm *tm, int tz);

AbsoluteTime
tm2abstime( struct tm *tm, int tz)
{
    int day, sec;

    /* validate, before going out of range on some members */
    if (tm->tm_year < 1901 || tm->tm_year > 2038
      || tm->tm_mon < 1 || tm->tm_mon > 12
      || tm->tm_mday < 1 || tm->tm_mday > 31
      || tm->tm_hour < 0 || tm->tm_hour >= 24
      || tm->tm_min < 0 || tm->tm_min > 59
      || tm->tm_sec < 0 || tm->tm_sec > 59)
	return INVALID_ABSTIME;

    day = (date2j( tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j( 1970, 1, 1));

    /* check for time out of range */
    if ((day < MIN_DAYNUM) || (day > MAX_DAYNUM))
	return INVALID_ABSTIME;

    /* convert to seconds */
    sec = tm->tm_sec + tz + (tm->tm_min +(day*24 + tm->tm_hour)*60)*60;

    /* check for overflow */
    if ((day == MAX_DAYNUM && sec < 0) ||
      (day == MIN_DAYNUM && sec > 0))
	return INVALID_ABSTIME;

    /* daylight correction */
    if (tm->tm_isdst < 0) {		/* unknown; find out */
	tm->tm_isdst = (CDayLight > 0);
    };
    if (tm->tm_isdst > 0)
	sec -= 60*60;

    /* check for reserved values (e.g. "current" on edge of usual range */
    if (!AbsoluteTimeIsReal(sec))
	return INVALID_ABSTIME;

    return sec;
} /* tm2abstime() */


/* nabstimein()
 * Decode date/time string and return abstime.
 */
AbsoluteTime
nabstimein(char* str)
{
    AbsoluteTime result;

    double fsec;
    int tz = 0;
    struct tm date, *tm = &date;

    char *field[MAXDATEFIELDS];
    char lowstr[MAXDATELEN+1];
    int dtype;
    int nf, ftype[MAXDATEFIELDS];

    if (!PointerIsValid(str))
	elog(WARN,"Bad (null) abstime external representation",NULL);

    if (strlen(str) > MAXDATELEN)
	elog( WARN, "Bad (length) abstime external representation '%s'",str);

    if ((ParseDateTime( str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
     || (DecodeDateTime( field, ftype, nf, &dtype, tm, &fsec, &tz) != 0))
	elog( WARN, "Bad abstime external representation '%s'",str);

#ifdef DATEDEBUG
printf( "nabstimein- %d fields are type %d (DTK_DATE=%d)\n", nf, dtype, DTK_DATE);
#endif

    switch (dtype) {
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
	elog(WARN,"Bad abstime (internal coding error) '%s'",str);
	result = INVALID_ABSTIME;
    };

    return result;
} /* nabstimein() */


/*
 * Given an AbsoluteTime return the English text version of the date
 */
char *
nabstimeout(AbsoluteTime time)
{
    /*
     * Fri Jan 28 23:05:29 1994 PST
     * 0        1         2
     * 12345678901234567890123456789
     *
     * we allocate some extra -- timezones are usually 3 characters but
     * this is not in the POSIX standard...
     */
    char buf[40];
    char* result;

    switch (time) {
    case EPOCH_ABSTIME:	  (void) strcpy(buf, EPOCH);	break;
    case INVALID_ABSTIME: (void) strcpy(buf, INVALID);	break;
    case CURRENT_ABSTIME: (void) strcpy(buf, DCURRENT);	break;
    case NOEND_ABSTIME:   (void) strcpy(buf, LATE);	break;
    case NOSTART_ABSTIME: (void) strcpy(buf, EARLY);	break;
    default:
	/* hack -- localtime happens to work for negative times */
	(void) strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Y %Z",
			localtime((time_t *) &time));
	break;
    }
    result = (char*)palloc(strlen(buf) + 1);
    strcpy(result, buf);
    return result;
}


/* turn a (struct tm) and a few variables into a time_t, with range checking */
AbsoluteTime
dateconv(register struct tm *tm, int zone)
{
    tm->tm_wday = tm->tm_yday = 0;

#if FALSE
    if (tm->tm_year < 70) {
	tm->tm_year += 2000;
    } else if (tm->tm_year < 1000) {
	tm->tm_year += 1900;
    };
#endif

    /* validate, before going out of range on some members */
    if (tm->tm_year < 1901 || tm->tm_year > 2038
      || tm->tm_mon < 1 || tm->tm_mon > 12
      || tm->tm_mday < 1 || tm->tm_mday > 31
      || tm->tm_hour < 0 || tm->tm_hour >= 24
      || tm->tm_min < 0 || tm->tm_min > 59
      || tm->tm_sec < 0 || tm->tm_sec > 59)
	return INVALID_ABSTIME;

    /*
     * zone should really be -zone, and tz should be set to tp->value, not
     * -tp->value.  Or the table could be fixed.
     */
    tm->tm_sec += zone;		/* mktime lets it be out of range */

    /* convert to seconds */
    return qmktime(tm);
}


/*
 * near-ANSI qmktime suitable for use by dateconv; not necessarily as paranoid
 * as ANSI requires, and it may not canonicalise the struct tm.  Ignores tm_wday
 * and tm_yday.
 */
time_t
qmktime(struct tm *tm)
{
    time_t sec;

    int day;

#if FALSE
    /* If it was a 2 digit year */
    if (tm->tm_year < 100)
	tm->tm_year += 1900;
#endif

    day = (date2j( tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j( 1970, 1, 1));

    /* check for time out of range */
    if ((day < MIN_DAYNUM) || (day > MAX_DAYNUM))
	return INVALID_ABSTIME;

    /* convert to seconds */
    sec = tm->tm_sec + (tm->tm_min +(day*24 + tm->tm_hour)*60)*60;

    /* check for overflow */
    if ((day == MAX_DAYNUM && sec < 0) ||
	(day == MIN_DAYNUM && sec > 0))
	return INVALID_ABSTIME;

    /* check for reserved values (e.g. "current" on edge of usual range */
    if (!AbsoluteTimeIsReal(sec))
	return INVALID_ABSTIME;

    /* daylight correction */
    if (tm->tm_isdst < 0) {		/* unknown; find out */
	tm->tm_isdst = (CDayLight > 0);
    };
    if (tm->tm_isdst > 0)
	sec -= 60*60;

    return sec;
} /* qmktime() */


/*
 *  AbsoluteTimeIsBefore -- true iff time1 is before time2.
 *  AbsoluteTimeIsBefore -- true iff time1 is after time2.
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

    return (time1 < time2);
}

bool
AbsoluteTimeIsAfter(AbsoluteTime time1, AbsoluteTime time2)
{
    Assert(AbsoluteTimeIsValid(time1));
    Assert(AbsoluteTimeIsValid(time2));

    if (time1 == CURRENT_ABSTIME)
	time1 = GetCurrentTransactionStartTime();

    if (time2 == CURRENT_ABSTIME)
	time2 = GetCurrentTransactionStartTime();

    return (time1 > time2);
}


/*
 *	abstimeeq	- returns 1, iff arguments are equal
 *	abstimene	- returns 1, iff arguments are not equal
 *	abstimelt	- returns 1, iff t1 less than t2
 *	abstimegt	- returns 1, iff t1 greater than t2
 *	abstimele	- returns 1, iff t1 less than or equal to t2
 *	abstimege	- returns 1, iff t1 greater than or equal to t2
 */
bool
abstimeeq(AbsoluteTime t1, AbsoluteTime t2)
{
    if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
	return 0;
    if (t1 == CURRENT_ABSTIME)
	t1 = GetCurrentTransactionStartTime();
    if (t2 == CURRENT_ABSTIME)
	t2 = GetCurrentTransactionStartTime();

    return(t1 == t2);
}

bool
abstimene(AbsoluteTime t1, AbsoluteTime t2)
{ 
    if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
	return 0;
    if (t1 == CURRENT_ABSTIME)
	t1 = GetCurrentTransactionStartTime();
    if (t2 == CURRENT_ABSTIME)
	t2 = GetCurrentTransactionStartTime();

    return(t1 != t2);
}

bool
abstimelt(AbsoluteTime t1, AbsoluteTime t2)
{ 
    if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
	return 0;
    if (t1 == CURRENT_ABSTIME)
	t1 = GetCurrentTransactionStartTime();
    if (t2 == CURRENT_ABSTIME)
	t2 = GetCurrentTransactionStartTime();

    return(t1 < t2);
}

bool
abstimegt(AbsoluteTime t1, AbsoluteTime t2)
{ 
    if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
	return 0;
    if (t1 == CURRENT_ABSTIME)
	t1 = GetCurrentTransactionStartTime();
    if (t2 == CURRENT_ABSTIME)
	t2 = GetCurrentTransactionStartTime();

    return(t1 > t2);
}

bool
abstimele(AbsoluteTime t1, AbsoluteTime t2)
{ 
    if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
	return 0;
    if (t1 == CURRENT_ABSTIME)
	t1 = GetCurrentTransactionStartTime();
    if (t2 == CURRENT_ABSTIME)
	t2 = GetCurrentTransactionStartTime();

    return(t1 <= t2);
}

bool
abstimege(AbsoluteTime t1, AbsoluteTime t2)
{ 
    if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
	return 0;
    if (t1 == CURRENT_ABSTIME)
	t1 = GetCurrentTransactionStartTime();
    if (t2 == CURRENT_ABSTIME)
	t2 = GetCurrentTransactionStartTime();

    return(t1 >= t2);
}

/* datetime_abstime()
 * Convert datetime to abstime.
 */
AbsoluteTime
datetime_abstime(DateTime *datetime)
{
    AbsoluteTime result;

    double fsec;
    struct tm tt, *tm = &tt;

    if (!PointerIsValid(datetime)) {
	result = INVALID_ABSTIME;

    } else if (DATETIME_IS_INVALID(*datetime)) {
	result = INVALID_ABSTIME;

    } else if (DATETIME_IS_NOBEGIN(*datetime)) {
	result = NOSTART_ABSTIME;

    } else if (DATETIME_IS_NOEND(*datetime)) {
	result = NOEND_ABSTIME;

    } else {
	if (DATETIME_IS_RELATIVE(*datetime)) {
	    datetime2tm( SetDateTime(*datetime), tm, &fsec);
	    result = tm2abstime( tm, 0);

	} else if (datetime2tm( *datetime, tm, &fsec) == 0) {
	    result = tm2abstime( tm, 0);

	} else {
	    result = INVALID_ABSTIME;
	};
    };

    return(result);
} /* datetime_abstime() */
