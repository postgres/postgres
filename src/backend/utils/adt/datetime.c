/*-------------------------------------------------------------------------
 *
 * datetime.c--
 *    implements DATE and TIME data types specified in SQL-92 standard
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/datetime.c,v 1.8 1997/06/03 13:56:32 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>		/* for sprintf() */
#include <string.h>
#include <limits.h>

#include "postgres.h"
#ifdef HAVE_FLOAT_H
#include <float.h>
#endif
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/nabstime.h"
#include "utils/datetime.h"
#include "access/xact.h"

static int	day_tab[2][12] = {
	{31,28,31,30,31,30,31,31,30,31,30,31},
	{31,29,31,30,31,30,31,31,30,31,30,31}  };

#define isleap(y) (((y % 4) == 0 && (y % 100) != 0) || (y % 400) == 0)

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
 *   Date ADT
 *****************************************************************************/


/* date_in()
 * Given date text string, convert to internal date format.
 */
#if USE_NEW_DATE

DateADT
date_in(char *str)
{
    DateADT date;

#else

int4
date_in(char *str)
{
    int4 result;
    DateADT *date = (DateADT *)&result;

#endif

    double fsec;
    struct tm tt, *tm = &tt;
    int tzp;
    int dtype;
    int nf;
    char *field[MAXDATEFIELDS];
    int ftype[MAXDATEFIELDS];
    char lowstr[MAXDATELEN+1];

    if (!PointerIsValid(str))
	elog(WARN,"Bad (null) date external representation",NULL);

#ifdef DATEDEBUG
printf( "date_in- input string is %s\n", str);
#endif
    if ((ParseDateTime( str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
      || (DecodeDateTime( field, ftype, nf, &dtype, tm, &fsec, &tzp) != 0))
	elog(WARN,"Bad date external representation %s",str);

    switch (dtype) {
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
	elog(WARN,"Unrecognized date external representation %s",str);
    };

    if (tm->tm_year < 0 || tm->tm_year > 32767)
	elog(WARN, "date_in: year must be limited to values 0 through 32767 in '%s'", str);
    if (tm->tm_mon < 1 || tm->tm_mon > 12)
	elog(WARN, "date_in: month must be limited to values 1 through 12 in '%s'", str);
    if (tm->tm_mday < 1 || tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon-1])
	elog(WARN, "date_in: day must be limited to values 1 through %d in '%s'",
	     day_tab[isleap(tm->tm_year)][tm->tm_mon-1], str);

#if USE_NEW_DATE

    date = (date2j( tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000,1,1));

    return(date);

#else

    date->day = tm->tm_mday;
    date->month = tm->tm_mon;
    date->year = tm->tm_year;

    return(result);

#endif
} /* date_in() */

/* date_out()
 * Given internal format date, convert to text string.
 */
#if USE_NEW_DATE

char *
date_out(DateADT date)
{

#else

char *
date_out(int4 dateVal)
{
    DateADT *date = (DateADT *)&dateVal;

#endif
    char *result;
    char buf[MAXDATELEN+1];
    int year, month, day;

#if USE_NEW_DATE

    j2date( (date + date2j(2000,1,1)), &year, &month, &day);

#else

    day = date->day;
    month = date->month;
    year = date->year;

#endif

    if (EuroDates == 1) /* Output European-format dates */
        sprintf(buf, "%02d-%02d-%04d", day, month, year);
    else
        sprintf(buf, "%02d-%02d-%04d", month, day, year);

    result = PALLOC(strlen(buf)+1);

    strcpy( result, buf);

    return(result);
} /* date_out() */

#if USE_NEW_DATE

int date2tm(DateADT dateVal, int *tzp, struct tm *tm, double *fsec, char **tzn);

bool
date_eq(DateADT dateVal1, DateADT dateVal2)
{
    return(dateVal1 == dateVal2);
}

bool
date_ne(DateADT dateVal1, DateADT dateVal2)
{
    return(dateVal1 != dateVal2);
}

bool
date_lt(DateADT dateVal1, DateADT dateVal2)
{
    return(dateVal1 < dateVal2);
} /* date_lt() */

bool
date_le(DateADT dateVal1, DateADT dateVal2)
{
    return(dateVal1 <= dateVal2);
} /* date_le() */

bool
date_gt(DateADT dateVal1, DateADT dateVal2)
{
    return(dateVal1 > dateVal2);
} /* date_gt() */

bool
date_ge(DateADT dateVal1, DateADT dateVal2)
{
    return(dateVal1 >= dateVal2);
} /* date_ge() */

int
date_cmp(DateADT dateVal1, DateADT dateVal2)
{
    if (dateVal1 < dateVal2) {
	return -1;
    } else if (dateVal1 > dateVal2) {
	return 1;
    };
    return 0;
} /* date_cmp() */

DateADT
date_larger(DateADT dateVal1, DateADT dateVal2)
{
  return(date_gt(dateVal1, dateVal2) ? dateVal1 : dateVal2);
} /* date_larger() */

DateADT
date_smaller(DateADT dateVal1, DateADT dateVal2)
{
  return(date_lt(dateVal1, dateVal2) ? dateVal1 : dateVal2);
} /* date_smaller() */

/* Compute difference between two dates in days.  */
int4
date_mi(DateADT dateVal1, DateADT dateVal2)
{
    return(dateVal1-dateVal2);
} /* date_mi() */

/* Add a number of days to a date, giving a new date.
   Must handle both positive and negative numbers of days.  */
DateADT
date_pli(DateADT dateVal, int4 days)
{
    return(dateVal+days);
} /* date_pli() */

/* Subtract a number of days from a date, giving a new date.  */
DateADT
date_mii(DateADT dateVal, int4 days)
{
  return(date_pli(dateVal, -days));
} /* date_mii() */


/* date_datetime()
 * Convert date to datetime data type.
 */
DateTime *
date_datetime(DateADT dateVal)
{
    DateTime *result;
    struct tm tt, *tm = &tt;
    int tz;
    double fsec = 0;
    char *tzn;

    result = PALLOCTYPE(DateTime);

    if (date2tm( dateVal, &tz, tm, &fsec, &tzn) != 0)
	elog(WARN,"Unable to convert date to datetime",NULL);

#ifdef DATEDEBUG
printf( "date_datetime- date is %d.%02d.%02d\n", tm->tm_year, tm->tm_mon, tm->tm_mday);
printf( "date_datetime- time is %02d:%02d:%02d %.7f\n", tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);
#endif

    if (tm2datetime( tm, fsec, &tz, result) != 0)
	elog(WARN,"Datetime out of range",NULL);

    return(result);
} /* date_datetime() */


/* datetime_date()
 * Convert datetime to date data type.
 */
DateADT
datetime_date(DateTime *datetime)
{
    DateADT result;
    struct tm tt, *tm = &tt;
    int tz;
    double fsec;
    char *tzn;

    if (!PointerIsValid(datetime))
	elog(WARN,"Unable to convert null datetime to date",NULL);

    if (DATETIME_NOT_FINITE(*datetime))
	elog(WARN,"Unable to convert datetime to date",NULL);

    if (DATETIME_IS_EPOCH(*datetime)) {
	datetime2tm( SetDateTime(*datetime), NULL, tm, &fsec, NULL);

    } else if (DATETIME_IS_CURRENT(*datetime)) {
	datetime2tm( SetDateTime(*datetime), &tz, tm, &fsec, &tzn);

    } else {
	if (datetime2tm( *datetime, &tz, tm, &fsec, &tzn) != 0)
	    elog(WARN,"Unable to convert datetime to date",NULL);
    };

    result = (date2j( tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j( 2000, 1, 1));

    return(result);
} /* datetime_date() */


/* abstime_date()
 * Convert abstime to date data type.
 */
DateADT
abstime_date(AbsoluteTime abstime)
{
    DateADT result;
    struct tm tt, *tm = &tt;
    int tz;

    switch (abstime) {
    case INVALID_ABSTIME:
    case NOSTART_ABSTIME:
    case NOEND_ABSTIME:
	elog(WARN,"Unable to convert reserved abstime value to date",NULL);
	/* pretend to drop through to make compiler think that result will be set */

    case EPOCH_ABSTIME:
	result = date2j(1970,1,1) - date2j(2000,1,1);
	break;

    case CURRENT_ABSTIME:
	GetCurrentTime(tm);
	result = date2j( tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000,1,1);
	break;

    default:
	abstime2tm(abstime, &tz, tm);
	result = date2j(tm->tm_year,tm->tm_mon,tm->tm_mday) - date2j(2000,1,1);
	break;
    };

    return(result);
} /* abstime_date() */


/* date2tm()
 * Convert date to time structure.
 * Note that date is an implicit local time, but the system calls assume
 *  that everything is GMT. So, convert to GMT, rotate to local time,
 *  and then convert again to try to get the time zones correct.
 */
int
date2tm(DateADT dateVal, int *tzp, struct tm *tm, double *fsec, char **tzn)
{
    struct tm *tx;
    time_t utime;
    *fsec = 0;

    j2date( (dateVal + date2j( 2000, 1, 1)), &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
    tm->tm_hour = 0;
    tm->tm_min = 0;
    tm->tm_sec = 0;
    tm->tm_isdst = -1;

    if (IS_VALID_UTIME( tm->tm_year, tm->tm_mon, tm->tm_mday)) {

	/* convert to system time */
	utime = ((dateVal + (date2j(2000,1,1)-date2j(1970,1,1)))*86400);
	utime += (12*60*60);	/* rotate to noon to get the right day in time zone */

#ifdef USE_POSIX_TIME
	tx = localtime(&utime);

#ifdef DATEDEBUG
#ifdef HAVE_INT_TIMEZONE
printf( "date2tm- (localtime) %d.%02d.%02d %02d:%02d:%02.0f %s %s dst=%d\n",
 tx->tm_year, tx->tm_mon, tx->tm_mday, tx->tm_hour, tx->tm_min, (double) tm->tm_sec,
 tzname[0], tzname[1], tx->tm_isdst);
#endif
#endif
	tm->tm_year = tx->tm_year + 1900;
	tm->tm_mon = tx->tm_mon + 1;
	tm->tm_mday = tx->tm_mday;
#if FALSE
	tm->tm_hour = tx->tm_hour;
	tm->tm_min = tx->tm_min;
	tm->tm_sec = tx->tm_sec;
#endif
	tm->tm_isdst = tx->tm_isdst;

#ifdef HAVE_INT_TIMEZONE
	*tzp = (tm->tm_isdst? (timezone - 3600): timezone);
	if (tzn != NULL) *tzn = tzname[(tm->tm_isdst > 0)];

#else /* !HAVE_INT_TIMEZONE */
	tm->tm_gmtoff = tx->tm_gmtoff;
	tm->tm_zone = tx->tm_zone;

	*tzp = (tm->tm_isdst? (tm->tm_gmtoff - 3600): tm->tm_gmtoff); /* tm_gmtoff is Sun/DEC-ism */
	if (tzn != NULL) *tzn = tm->tm_zone;
#endif

#else /* !USE_POSIX_TIME */
	*tzp = CTimeZone;	/* V7 conventions; don't know timezone? */
	if (tzn != NULL) *tzn = CTZName;
#endif

    /* otherwise, outside of timezone range so convert to GMT... */
    } else {
#if FALSE
	j2date( (dateVal + date2j( 2000, 1, 1)), &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
#endif

#ifdef DATEDEBUG
printf( "date2tm- convert %d-%d-%d %d:%d%d to datetime\n",
 tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif

	*tzp = 0;
	tm->tm_isdst = 0;
	if (tzn != NULL) *tzn = NULL;
    };

    return 0;
} /* date2tm() */

#else

bool
date_eq(int4 dateVal1, int4 dateVal2)
{
    int4 dateStore1 = dateVal1;
    int4 dateStore2 = dateVal2;
    DateADT *date1, *date2;
    
    date1 = (DateADT*)&dateStore1;
    date2 = (DateADT*)&dateStore2;

    return (date1->day==date2->day && 
	    date1->month==date2->month &&
	    date1->year==date2->year);
}

bool
date_ne(int4 dateVal1, int4 dateVal2)
{
    int4 dateStore1 = dateVal1;
    int4 dateStore2 = dateVal2;
    DateADT *date1, *date2;
    
    date1 = (DateADT*)&dateStore1;
    date2 = (DateADT*)&dateStore2;

    return (date1->day!=date2->day || date1->month!=date2->month ||
	    date1->year!=date2->year);
}

bool
date_lt(int4 dateVal1, int4 dateVal2)
{
    int4 dateStore1 = dateVal1;
    int4 dateStore2 = dateVal2;
    DateADT *date1, *date2;
    
    date1 = (DateADT*)&dateStore1;
    date2 = (DateADT*)&dateStore2;

    if (date1->year!=date2->year)
	return (date1->year<date2->year);
    if (date1->month!=date2->month)
	return (date1->month<date2->month);
    return (date1->day<date2->day);
}

bool
date_le(int4 dateVal1, int4 dateVal2)
{

    int4 dateStore1 = dateVal1;
    int4 dateStore2 = dateVal2;
    DateADT *date1, *date2;
    
    date1 = (DateADT*)&dateStore1;
    date2 = (DateADT*)&dateStore2;

    if (date1->year!=date2->year)
	return (date1->year<=date2->year);
    if (date1->month!=date2->month)
	return (date1->month<=date2->month);
    return (date1->day<=date2->day);
}

bool
date_gt(int4 dateVal1, int4 dateVal2)
{
    int4 dateStore1 = dateVal1;
    int4 dateStore2 = dateVal2;
    DateADT *date1, *date2;
    
    date1 = (DateADT*)&dateStore1;
    date2 = (DateADT*)&dateStore2;


    if (date1->year!=date2->year)
	return (date1->year>date2->year);
    if (date1->month!=date2->month)
	return (date1->month>date2->month);
    return (date1->day>date2->day);
}

bool
date_ge(int4 dateVal1, int4 dateVal2)
{
    int4 dateStore1 = dateVal1;
    int4 dateStore2 = dateVal2;
    DateADT *date1, *date2;
    
    date1 = (DateADT*)&dateStore1;
    date2 = (DateADT*)&dateStore2;

    if (date1->year!=date2->year)
	return (date1->year>=date2->year);
    if (date1->month!=date2->month)
	return (date1->month>=date2->month);
    return (date1->day>=date2->day);
}

int
date_cmp(int4 dateVal1, int4 dateVal2)
{
    int4 dateStore1 = dateVal1;
    int4 dateStore2 = dateVal2;
    DateADT *date1, *date2;
    
    date1 = (DateADT*)&dateStore1;
    date2 = (DateADT*)&dateStore2;

    if (date1->year!=date2->year)
	return ((date1->year<date2->year) ? -1 : 1);
    if (date1->month!=date2->month)
	return ((date1->month<date2->month) ? -1 : 1);
    if (date1->day!=date2->day)
	return ((date1->day<date2->day) ? -1 : 1);
    return 0;
}

int4
date_larger(int4 dateVal1, int4 dateVal2)
{
  return (date_gt (dateVal1, dateVal2) ? dateVal1 : dateVal2);
}

int4
date_smaller(int4 dateVal1, int4 dateVal2)
{
  return (date_lt (dateVal1, dateVal2) ? dateVal1 : dateVal2);
}

/* Compute difference between two dates in days.  */
int32
date_mi(int4 dateVal1, int4 dateVal2)
{
    DateADT *date1, *date2;
    int days;

    date1 = (DateADT *) &dateVal1;
    date2 = (DateADT *) &dateVal2;

    days = (date2j(date1->year, date1->month, date1->day)
          - date2j(date2->year, date2->month, date2->day));

  return (days);
}

/* Add a number of days to a date, giving a new date.
   Must handle both positive and negative numbers of days.  */
int4
date_pli(int4 dateVal, int32 days)
{
    DateADT *date1 = (DateADT *) &dateVal;
    int date, year, month, day;

    date = (date2j(date1->year, date1->month, date1->day) + days);
    j2date( date, &year, &month, &day);
    date1->year = year;
    date1->month = month;
    date1->day = day;

    return (dateVal);
} /* date_pli() */

/* Subtract a number of days from a date, giving a new date.  */
int4
date_mii(int4 dateVal, int32 days)
{
    return (date_pli(dateVal, -days));
}

DateTime *
date_datetime(int4 dateVal)
{
    DateTime *result;
    DateADT *date = (DateADT *) &dateVal;

    int tz;
    double fsec;
    char *tzn;
    struct tm tt, *tm = &tt;

    result = PALLOCTYPE(DateTime);

    *result = (date2j(date->year, date->month, date->day) - date2j( 2000, 1, 1));
    *result *= 86400;
    *result += (12*60*60);

    datetime2tm( *result, &tz, tm, &fsec, &tzn);
    tm->tm_hour = 0;
    tm->tm_min = 0;
    tm->tm_sec = 0;
    tm2datetime( tm, fsec, &tz, result);

#ifdef DATEDEBUG
printf( "date_datetime- convert %04d-%02d-%02d to %f\n",
 date->year, date->month, date->day, *result);
#endif

    return(result);
} /* date_datetime() */

int4
datetime_date(DateTime *datetime)
{
    int4 result;
    int tz;
    double fsec;
    char *tzn;
    struct tm tt, *tm = &tt;
    DateADT *date = (DateADT *) &result;

    if (!PointerIsValid(datetime))
	elog(WARN,"Unable to convert null datetime to date",NULL);

    if (DATETIME_NOT_FINITE(*datetime))
	elog(WARN,"Unable to convert datetime to date",NULL);

    if (DATETIME_IS_EPOCH(*datetime)) {
	datetime2tm( SetDateTime(*datetime), NULL, tm, &fsec, NULL);

    } else if (DATETIME_IS_CURRENT(*datetime)) {
	datetime2tm( SetDateTime(*datetime), &tz, tm, &fsec, &tzn);

    } else {
	if (datetime2tm( *datetime, &tz, tm, &fsec, &tzn) != 0)
	    elog(WARN,"Unable to convert datetime to date",NULL);
    };

    date->year = tm->tm_year;
    date->month = tm->tm_mon;
    date->day = tm->tm_mday;

    return(result);
} /* datetime_date() */

int4
abstime_date(AbsoluteTime abstime)
{
    int4 result;
    DateADT *date = (DateADT *) &result;
    int tz;
    struct tm tt, *tm = &tt;

    switch (abstime) {
    case INVALID_ABSTIME:
    case NOSTART_ABSTIME:
    case NOEND_ABSTIME:
	elog(WARN,"Unable to convert reserved abstime value to date",NULL);
	break;

    case EPOCH_ABSTIME:
	date->year = 1970;
	date->month = 1;
	date->day = 1;
	break;

    case CURRENT_ABSTIME:
	abstime = GetCurrentTransactionStartTime();
	abstime2tm(abstime, &tz, tm);
	date->year = tm->tm_year;
	date->month = tm->tm_mon;
	date->day = tm->tm_mday;
	break;

    default:
#if FALSE
	tm = localtime((time_t *) &abstime);
	tm->tm_year += 1900;
	tm->tm_mon += 1;
#endif
	abstime2tm(abstime, &tz, tm);
	date->year = tm->tm_year;
	date->month = tm->tm_mon;
	date->day = tm->tm_mday;
	break;
    };

    return(result);
} /* abstime_date() */

#endif


/*****************************************************************************
 *   Time ADT
 *****************************************************************************/


TimeADT *
time_in(char *str)
{
    TimeADT *time;

    double fsec;
    struct tm tt, *tm = &tt;

    int nf;
    char lowstr[MAXDATELEN+1];
    char *field[MAXDATEFIELDS];
    int dtype;
    int ftype[MAXDATEFIELDS];

    if (!PointerIsValid(str))
        elog(WARN,"Bad (null) time external representation",NULL);

    if ((ParseDateTime( str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
     || (DecodeTimeOnly( field, ftype, nf, &dtype, tm, &fsec) != 0))
        elog(WARN,"Bad time external representation '%s'",str);

    if ((tm->tm_hour < 0) || (tm->tm_hour > 23))
	elog(WARN,"Hour must be limited to values 0 through 23 in '%s'",str);
    if ((tm->tm_min < 0) || (tm->tm_min > 59))
	elog(WARN,"Minute must be limited to values 0 through 59 in '%s'",str);
    if ((tm->tm_sec < 0) || ((tm->tm_sec + fsec) >= 60))
	elog(WARN,"Second must be limited to values 0 through < 60 in '%s'",str);

    time = PALLOCTYPE(TimeADT);

#if USE_NEW_TIME

    *time = ((((tm->tm_hour*60)+tm->tm_min)*60)+tm->tm_sec+fsec);

#else

    time->hr = tm->tm_hour;
    time->min = tm->tm_min;
    time->sec = (tm->tm_sec + fsec);

#endif

    return(time);
} /* time_in() */


char *
time_out(TimeADT *time)
{
    char *result;
#if USE_NEW_TIME
    int hour, min, sec;
    double fsec;
#endif
    char buf[32];

    if (!PointerIsValid(time))
	return NULL;

#if USE_NEW_TIME

    hour = (*time / (60*60));
    min = (((int) (*time / 60)) % 60);
    sec = (((int) *time) % 60);

    fsec = 0;

    if (sec == 0.0) {
	sprintf(buf, "%02d:%02d", hour, min);

    } else {
	if (fsec == 0) {
	    sprintf(buf, "%02d:%02d:%02d", hour, min, sec);
	} else {
	    sprintf(buf, "%02d:%02d:%05.2f", hour, min, (sec+fsec));
	};
    };

#else

    if (time->sec == 0.0) {
	sprintf(buf, "%02d:%02d", (int)time->hr, (int)time->min);
    } else {
	if (((int) time->sec) == time->sec) {
	    sprintf(buf, "%02d:%02d:%02d",
	      (int)time->hr, (int)time->min, (int)time->sec);
	} else {
	    sprintf(buf, "%02d:%02d:%09.6f",
	      (int)time->hr, (int)time->min, time->sec);
	};
    };

#endif

    result = PALLOC(strlen(buf)+1);

    strcpy( result, buf);

    return(result);
} /* time_out() */


#if USE_NEW_TIME

bool
time_eq(TimeADT *time1, TimeADT *time2)
{
    if (!PointerIsValid(time1) || !PointerIsValid(time2))
	return(FALSE);

    return(*time1 == *time2);
} /* time_eq() */

bool
time_ne(TimeADT *time1, TimeADT *time2)
{
    if (!PointerIsValid(time1) || !PointerIsValid(time2))
	return(FALSE);

    return(*time1 != *time2);
} /* time_eq() */

bool
time_lt(TimeADT *time1, TimeADT *time2)
{
    if (!PointerIsValid(time1) || !PointerIsValid(time2))
	return(FALSE);

    return(*time1 < *time2);
} /* time_eq() */

bool
time_le(TimeADT *time1, TimeADT *time2)
{
    if (!PointerIsValid(time1) || !PointerIsValid(time2))
	return(FALSE);

    return(*time1 <= *time2);
} /* time_eq() */

bool
time_gt(TimeADT *time1, TimeADT *time2)
{
    if (!PointerIsValid(time1) || !PointerIsValid(time2))
	return(FALSE);

    return(*time1 > *time2);
} /* time_eq() */

bool
time_ge(TimeADT *time1, TimeADT *time2)
{
    if (!PointerIsValid(time1) || !PointerIsValid(time2))
	return(FALSE);

    return(*time1 >= *time2);
} /* time_eq() */

int
time_cmp(TimeADT *time1, TimeADT *time2)
{
    return((*time1 < *time2)? -1: (((*time1 < *time2)? 1: 0)));
} /* time_cmp() */


/* datetime_datetime()
 * Convert date and time to datetime data type.
 */
DateTime *
datetime_datetime(DateADT date, TimeADT *time)
{
    DateTime *result;

    if (!PointerIsValid(time)) {
	result = PALLOCTYPE(DateTime);
	DATETIME_INVALID(*result);

    } else {
	result = date_datetime(date);
	*result += *time;
    };

    return(result);
} /* datetime_datetime() */

#else

bool
time_eq(TimeADT *time1, TimeADT *time2)
{
    return (time1->sec==time2->sec && time1->min==time2->min &&
	    time1->hr==time2->hr);
}

bool
time_ne(TimeADT *time1, TimeADT *time2)
{
    return (time1->sec!=time2->sec || time1->min!=time2->min ||
	    time1->hr!=time2->hr);
}

bool
time_lt(TimeADT *time1, TimeADT *time2)
{
    if (time1->hr!=time2->hr)
	return (time1->hr<time2->hr);
    if (time1->min!=time2->min)
	return (time1->min<time2->min);
    return (time1->sec<time2->sec);
}

bool
time_le(TimeADT *time1, TimeADT *time2)
{
    if (time1->hr!=time2->hr)
	return (time1->hr<=time2->hr);
    if (time1->min!=time2->min)
	return (time1->min<=time2->min);
    return (time1->sec<=time2->sec);
}

bool
time_gt(TimeADT *time1, TimeADT *time2)
{
    if (time1->hr!=time2->hr)
	return (time1->hr>time2->hr);
    if (time1->min!=time2->min)
	return (time1->min>time2->min);
    return (time1->sec>time2->sec);
}

bool
time_ge(TimeADT *time1, TimeADT *time2)
{
    if (time1->hr!=time2->hr)
	return (time1->hr>=time2->hr);
    if (time1->min!=time2->min)
	return (time1->min>=time2->min);
    return (time1->sec>=time2->sec);
}

int
time_cmp(TimeADT *time1, TimeADT *time2)
{
    if (time1->hr!=time2->hr)
	return ((time1->hr<time2->hr) ? -1 : 1);
    if (time1->min!=time2->min)
	return ((time1->min<time2->min) ? -1 : 1);
    if (time1->sec!=time2->sec)
	return ((time1->sec<time2->sec) ? -1 : 1);
    return 0;
}

DateTime *
datetime_datetime(int4 dateVal, TimeADT *time)
{
    DateTime *result;
#ifdef DATEDEBUG
    DateADT *date = (DateADT *) &dateVal;
#endif

    if (!PointerIsValid(time)) {
	result = PALLOCTYPE(DateTime);
	DATETIME_INVALID(*result);

    } else {

#ifdef DATEDEBUG
printf( "datetime_datetime- convert %04d-%02d-%02d %02d:%02d:%05.2f\n",
 date->year, date->month, date->day, time->hr, time->min, time->sec);
#endif

	result = date_datetime(dateVal);
	*result += (((time->hr*60)+time->min)*60+time->sec);
    };

    return(result);
} /* datetime_datetime() */

#endif

int32   /* RelativeTime */
int42reltime(int32 timevalue)
{
    return(timevalue);
}
