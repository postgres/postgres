/*-------------------------------------------------------------------------
 *
 * datetime.c--
 *    implements DATE and TIME data types specified in SQL-92 standard
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/datetime.c,v 1.4 1997/04/17 13:50:34 scrappy Exp $
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

    date = (date2j(tm->tm_year,tm->tm_mon,tm->tm_mday) - date2j(2000,1,1));

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

    j2date( (((int) date) + date2j(2000,1,1)), &year, &month, &day);

#else

    day = date->day;
    month = date->month;
    year = date->year;

#endif

    if (EuroDates == 1) /* Output European-format dates */
        sprintf(buf, "%02d-%02d-%04d", day, month, year);
    else
        sprintf(buf, "%02d-%02d-%04d", month, day, year);

    if (!PointerIsValid(result = PALLOC(strlen(buf)+1)))
	elog(WARN,"Memory allocation failed, can't output date",NULL);

    strcpy( result, buf);

    return(result);
} /* date_out() */

#if USE_NEW_DATE

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
date_datetime(int4 dateVal)
{
    DateTime *result;

    if (!PointerIsValid(result = PALLOCTYPE(DateTime)))
	elog(WARN,"Memory allocation failed, can't convert date to datetime",NULL);

    *result = (dateVal - date2j( 2000, 1, 1));
    *result *= 86400;

    return(result);
} /* date_datetime() */


/* datetime_date()
 * Convert datetime to date data type.
 */
DateADT
datetime_date(DateTime *datetime)
{
    DateADT result;

    if (!PointerIsValid(datetime))
	elog(WARN,"Unable to convert null datetime to date",NULL);

    result = (*datetime / 86400);

    return(result);
} /* datetime_date() */


/* abstime_date()
 * Convert abstime to date data type.
 */
DateADT
abstime_date(AbsoluteTime abstime)
{
    DateADT result;
    struct tt, *tm = &tt;

    switch (abstime) {
    case INVALID_ABSTIME:
    case NOSTART_ABSTIME:
    case NOEND_ABSTIME:
	elog(WARN,"Unable to convert reserved abstime value to date",NULL);
	break;

    case EPOCH_ABSTIME:
	result = date2j(1970,1,1) - date2j(2000,1,1);
	break;

    case CURRENT_ABSTIME:
	GetCurrentTime(tm);
	result = date2j(tm->tm_year,tm->tm_mon,tm->tm_mday) - date2j(2000,1,1);
	break;

    default:
#if FALSE
	tm = localtime((time_t *) &abstime);
	tm->tm_year += 1900;
	tm->tm_mon += 1;
	/* XXX must shift to local time before converting - tgl 97/04/01 */
#endif
	abstime2tm(abstime, &CTimeZone, tm);
	result = date2j(tm->tm_year,tm->tm_mon,tm->tm_mday) - date2j(2000,1,1);
	break;
    };

    return(result);
} /* abstime_date() */

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

    if (!PointerIsValid(result = PALLOCTYPE(DateTime)))
	elog(WARN,"Memory allocation failed, can't convert date to datetime",NULL);

    *result = (date2j(date->year, date->month, date->day) - date2j( 2000, 1, 1));
    *result *= 86400;

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
    int year, month, day;
    DateADT *date = (DateADT *) &result;

    if (!PointerIsValid(datetime))
	elog(WARN,"Unable to convert null datetime to date",NULL);

    j2date( ((*datetime / 86400) + date2j( 2000, 1, 1)), &year, &month, &day);
    date->year = year;
    date->month = month;
    date->day = day;

    return(result);
} /* datetime_date() */

int4
abstime_date(AbsoluteTime abstime)
{
    int4 result;
    DateADT *date = (DateADT *) &result;
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
#if FALSE
	GetCurrentTime(tm);
#endif
	abstime = GetCurrentTransactionStartTime() + CTimeZone;
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
	abstime2tm(abstime, &CTimeZone, tm);
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

    if (!PointerIsValid(time = PALLOCTYPE(TimeADT)))
	elog(WARN,"Memory allocation failed, can't input time '%s'",str);

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
    char buf[32];

    if (!PointerIsValid(time))
	return NULL;

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

    if (!PointerIsValid(result = PALLOC(strlen(buf)+1)))
	elog(WARN,"Memory allocation failed, can't output time",NULL);

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
	if (!PointerIsValid(result = PALLOCTYPE(DateTime)))
	    elog(WARN,"Memory allocation failed, can't convert to datetime",NULL);

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
	if (!PointerIsValid(result = PALLOCTYPE(DateTime)))
	    elog(WARN,"Memory allocation failed, can't convert to datetime",NULL);

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
