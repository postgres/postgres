/*-------------------------------------------------------------------------
 *
 * dt.c--
 *    Functions for the built-in type "dt".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/dt.c,v 1.10 1997/03/25 20:02:42 scrappy Exp $
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
#include <miscadmin.h>
#ifdef HAVE_FLOAT_H
# include <float.h>
#endif
#ifndef USE_POSIX_TIME
#include <sys/timeb.h>
#endif
#include "utils/builtins.h"

#define USE_DATE_CACHE 1

#if FALSE
#ifdef NAN
#define DT_INVALID		(NAN)
#else
#define DT_INVALID		(DBL_MIN+DBL_MIN)
#endif
#ifdef HUGE_VAL
#define DT_NOBEGIN		(-HUGE_VAL)
#define DT_NOEND		(HUGE_VAL)
#else
#define DT_NOBEGIN		(-DBL_MAX)
#define DT_NOEND		(DBL_MAX)
#endif
#define DT_CURRENT		(DBL_MIN)
#define DT_EPOCH		(-DBL_MIN)

#define DATETIME_INVALID(j)	{j = DT_INVALID;}
#ifdef NAN
#define DATETIME_IS_INVALID(j)	(isnan(j))
#else
#define DATETIME_IS_INVALID(j)	(j == DT_INVALID)
#endif

#define DATETIME_NOBEGIN(j)	{j = DT_NOBEGIN;}
#define DATETIME_IS_NOBEGIN(j)	(j == DT_NOBEGIN)

#define DATETIME_NOEND(j)	{j = DT_NOEND;}
#define DATETIME_IS_NOEND(j)	(j == DT_NOEND)

#define DATETIME_CURRENT(j)	{j = DT_CURRENT;}
#define DATETIME_IS_CURRENT(j)	(j == DT_CURRENT)

#define DATETIME_EPOCH(j)	{j = DT_EPOCH;}
#define DATETIME_IS_EPOCH(j)	(j == DT_EPOCH)

#define DATETIME_IS_RELATIVE(j)	(DATETIME_IS_CURRENT(j) || DATETIME_IS_EPOCH(j))
#define DATETIME_NOT_FINITE(j)	(DATETIME_IS_INVALID(j) \
				 || DATETIME_IS_NOBEGIN(j) || DATETIME_IS_NOEND(j))
#define DATETIME_IS_RESERVED(j)	(DATETIME_IS_RELATIVE(j) || DATETIME_NOT_FINITE(j))


#define TIMESPAN_INVALID(j)	{j->time = DT_INVALID;}
#ifdef NAN
#define TIMESPAN_IS_INVALID(j)	(isnan((j).time))
#else
#define TIMESPAN_IS_INVALID(j)	((j).time == DT_INVALID)
#endif

#define TIME_PREC 1e-6
#define JROUND(j) (rint(((double) j)/TIME_PREC)*TIME_PREC)
#endif


/***************************************************************************** 
 *   USER I/O ROUTINES                                                       *
 *****************************************************************************/

/* datetime_in()
 * Convert a string to internal form.
 */
DateTime *
datetime_in(char *str)
{
    DateTime *result;

#if FALSE
    double date, time;
#endif
    double fsec;
    struct tm tt, *tm = &tt;
    int tzp;
    int dtype;
    int nf;
    char *field[MAXDATEFIELDS];
    int ftype[MAXDATEFIELDS];
    char lowstr[MAXDATELEN];

    if (!PointerIsValid(str))
	elog(WARN, "Bad (null) datetime external representation", NULL);

    if ((ParseDateTime( str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
      || (DecodeDateTime( field, ftype, nf, &dtype, tm, &fsec, &tzp) != 0))
	elog(WARN,"Bad datetime external representation %s",str);

    if (!PointerIsValid(result = PALLOCTYPE(DateTime)))
	elog(WARN, "Memory allocation failed, can't input datetime '%s'",str);

    switch (dtype) {
    case DTK_DATE:
#if FALSE
	if (tzp != 0) {
	    *result = tm2datetime( tm, fsec, tzp);
	} else {
	    *result = tm2datetime( tm, fsec, CTimeZone);
	};
#endif
	*result = tm2datetime( tm, fsec, tzp);

#ifdef DATEDEBUG
printf( "datetime_in- date is %f\n", *result);
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
	elog(WARN, "Internal coding error, can't input datetime '%s'",str);
    };

    return(result);
} /* datetime_in() */

/* datetime_out()
 * Convert a datetime to external form.
 */
char *
datetime_out(DateTime *dt)
{
    char *result;
    struct tm tt, *tm = &tt;
    double fsec;
    char buf[MAXDATELEN];

    if (!PointerIsValid(dt))
	return(NULL);

    if (DATETIME_IS_RESERVED(*dt)) {
	EncodeSpecialDateTime(*dt, buf);

    } else if (datetime2tm( *dt, tm, &fsec) == 0) {
	EncodePostgresDate(tm, fsec, buf);

    } else {
	EncodeSpecialDateTime(DT_INVALID, buf);
    };

    if (!PointerIsValid(result = PALLOC(strlen(buf)+1)))
	elog(WARN, "Memory allocation failed, can't output datetime", NULL);

    strcpy( result, buf);

    return( result);
} /* datetime_out() */


/* timespan_in()
 * Convert a string to internal form.
 *
 * External format(s):
 *  Uses the generic date/time parsing and decoding routines.
 */
TimeSpan *
timespan_in(char *str)
{
    TimeSpan *span;

    double fsec;
    struct tm tt, *tm = &tt;
    int dtype;
    int nf;
    char *field[MAXDATEFIELDS];
    int ftype[MAXDATEFIELDS];
    char lowstr[MAXDATELEN];

    tm->tm_year = 0;
    tm->tm_mon = 0;
    tm->tm_mday = 0;
    tm->tm_hour = 0;
    tm->tm_min = 0;
    tm->tm_sec = 0;
    fsec = 0;

    if (!PointerIsValid(str))
	elog(WARN, "Bad (null) timespan external representation", NULL);

    if ((ParseDateTime( str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
      || (DecodeDateDelta( field, ftype, nf, &dtype, tm, &fsec) != 0))
	elog(WARN,"Bad timespan external representation %s",str);

    if (!PointerIsValid(span = PALLOCTYPE(TimeSpan)))
	elog(WARN, "Memory allocation failed, can't input timespan '%s'",str);

    switch (dtype) {
    case DTK_DELTA:
	if (tm2timespan(tm, fsec, span) != 0) {
#if FALSE
	    TIMESPAN_INVALID(span);
#endif
	};
	break;

    default:
	elog(WARN, "Internal coding error, can't input timespan '%s'",str);
    };

    return(span);
} /* timespan_in() */

/* timespan_out()
 * Convert a time span to external form.
 */
char *
timespan_out(TimeSpan *span)
{
    char *result;

    struct tm tt, *tm = &tt;
    double fsec;
    char buf[MAXDATELEN];

    if (!PointerIsValid(span))
	return(NULL);

    if (timespan2tm(*span, tm, &fsec) != 0)
	return(NULL);

    if (EncodePostgresSpan(tm, fsec, buf) != 0)
	  elog(WARN,"Unable to format timespan",NULL);

    if (!PointerIsValid(result = PALLOC(strlen(buf)+1)))
	elog(WARN,"Memory allocation failed, can't output timespan",NULL);

    strcpy( result, buf);
    return( result);
} /* timespan_out() */


/***************************************************************************** 
 *   PUBLIC ROUTINES                                                         *
 *****************************************************************************/


/*----------------------------------------------------------
 *  Relational operators for datetime.
 *---------------------------------------------------------*/

void GetEpochTime( struct tm *tm);

void
GetEpochTime( struct tm *tm)
{
    struct tm *t0;
    time_t epoch = 0;

    t0 = gmtime( &epoch);

    tm->tm_year = t0->tm_year;
    tm->tm_mon = t0->tm_mon;
    tm->tm_mday = t0->tm_mday;
    tm->tm_hour = t0->tm_hour;
    tm->tm_min = t0->tm_min;
    tm->tm_sec = t0->tm_sec;

    if (tm->tm_year < 1900) tm->tm_year += 1900;

#ifdef DATEDEBUG
printf( "GetEpochTime- %04d-%02d-%02d %02d:%02d:%02d\n",
 tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif

    return;
} /* GetEpochTime() */

DateTime
SetDateTime( DateTime dt) {
    struct tm tt;

    if (DATETIME_IS_CURRENT(dt)) {
	GetCurrentTime(&tt);
	dt = tm2datetime( &tt, 0, 0);

#ifdef DATEDEBUG
printf( "SetDateTime- current time is %f\n", dt);
#endif
    } else { /* if (DATETIME_IS_EPOCH(dt1)) */
	GetEpochTime(&tt);
	dt = tm2datetime( &tt, 0, 0);
#ifdef DATEDEBUG
printf( "SetDateTime- epoch time is %f\n", dt);
#endif
    };

    return(dt);
} /* SetDateTime() */

/*	datetime_relop	- is datetime1 relop datetime2
 */
bool
datetime_eq(DateTime *datetime1, DateTime *datetime2)
{
    DateTime dt1, dt2;

    if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
	return FALSE;

    dt1 = *datetime1;
    dt2 = *datetime2;

#if FALSE
    if (DATETIME_NOT_FINITE(dt1) || DATETIME_NOT_FINITE(dt2))
	return FALSE;
#endif
    if (DATETIME_IS_INVALID(dt1) || DATETIME_IS_INVALID(dt2))
	return FALSE;

    if (DATETIME_IS_RELATIVE(dt1)) dt1 = SetDateTime(dt1);
    if (DATETIME_IS_RELATIVE(dt2)) dt2 = SetDateTime(dt2);

    return( dt1 == dt2);
} /* datetime_eq() */

bool
datetime_ne(DateTime *datetime1, DateTime *datetime2)
{
    DateTime dt1, dt2;

    if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
	return FALSE;

    dt1 = *datetime1;
    dt2 = *datetime2;

#if FALSE
    if (DATETIME_NOT_FINITE(dt1) || DATETIME_NOT_FINITE(dt2))
	return FALSE;
#endif
    if (DATETIME_IS_INVALID(dt1) || DATETIME_IS_INVALID(dt2))
	return FALSE;

    if (DATETIME_IS_RELATIVE(dt1)) dt1 = SetDateTime(dt1);
    if (DATETIME_IS_RELATIVE(dt2)) dt2 = SetDateTime(dt2);

    return( dt1 != dt2);
} /* datetime_ne() */

bool
datetime_lt(DateTime *datetime1, DateTime *datetime2)
{
    DateTime dt1, dt2;

    if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
	return FALSE;

    dt1 = *datetime1;
    dt2 = *datetime2;

#if FALSE
    if (DATETIME_NOT_FINITE(dt1) || DATETIME_NOT_FINITE(dt2))
	return FALSE;
#endif
    if (DATETIME_IS_INVALID(dt1) || DATETIME_IS_INVALID(dt2))
	return FALSE;

    if (DATETIME_IS_RELATIVE(dt1)) dt1 = SetDateTime(dt1);
    if (DATETIME_IS_RELATIVE(dt2)) dt2 = SetDateTime(dt2);

    return( dt1 < dt2);
} /* datetime_lt() */

bool
datetime_gt(DateTime *datetime1, DateTime *datetime2)
{
    DateTime dt1, dt2;

    if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
	return FALSE;

    dt1 = *datetime1;
    dt2 = *datetime2;

#if FALSE
    if (DATETIME_NOT_FINITE(dt1) || DATETIME_NOT_FINITE(dt2))
	return FALSE;
#endif
    if (DATETIME_IS_INVALID(dt1) || DATETIME_IS_INVALID(dt2))
	return FALSE;

    if (DATETIME_IS_RELATIVE(dt1)) dt1 = SetDateTime(dt1);
    if (DATETIME_IS_RELATIVE(dt2)) dt2 = SetDateTime(dt2);

#ifdef DATEDEBUG
printf( "datetime_gt- %f %s greater than %f\n", dt1, ((dt1 > dt2)? "is": "is not"), dt2);
#endif
    return( dt1 > dt2);
} /* datetime_gt() */

bool
datetime_le(DateTime *datetime1, DateTime *datetime2)
{
    DateTime dt1, dt2;

    if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
	return FALSE;

    dt1 = *datetime1;
    dt2 = *datetime2;

#if FALSE
    if (DATETIME_NOT_FINITE(dt1) || DATETIME_NOT_FINITE(dt2))
	return FALSE;
#endif
    if (DATETIME_IS_INVALID(dt1) || DATETIME_IS_INVALID(dt2))
	return FALSE;

    if (DATETIME_IS_RELATIVE(dt1)) dt1 = SetDateTime(dt1);
    if (DATETIME_IS_RELATIVE(dt2)) dt2 = SetDateTime(dt2);

    return( dt1 <= dt2);
} /* datetime_le() */

bool
datetime_ge(DateTime *datetime1, DateTime *datetime2)
{
    DateTime dt1, dt2;

    if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
	return FALSE;

    dt1 = *datetime1;
    dt2 = *datetime2;

#if FALSE
    if (DATETIME_NOT_FINITE(dt1) || DATETIME_NOT_FINITE(dt2))
	return FALSE;
#endif
    if (DATETIME_IS_INVALID(dt1) || DATETIME_IS_INVALID(dt2))
	return FALSE;

    if (DATETIME_IS_RELATIVE(dt1)) dt1 = SetDateTime(dt1);
    if (DATETIME_IS_RELATIVE(dt2)) dt2 = SetDateTime(dt2);

    return( dt1 >= dt2);
} /* datetime_ge() */


/*	timespan_relop	- is timespan1 relop timespan2
 */
bool
timespan_eq(TimeSpan *timespan1, TimeSpan *timespan2)
{
    if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
	return FALSE;

    if (TIMESPAN_IS_INVALID(*timespan1) || TIMESPAN_IS_INVALID(*timespan2))
	return FALSE;

    return( (timespan1->time == timespan2->time)
         && (timespan1->month == timespan2->month));
} /* timespan_eq() */

bool
timespan_ne(TimeSpan *timespan1, TimeSpan *timespan2)
{
    if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
	return FALSE;

    return( (timespan1->time != timespan2->time)
         || (timespan1->month != timespan2->month));
} /* timespan_ne() */

bool
timespan_lt(TimeSpan *timespan1, TimeSpan *timespan2)
{
    double span1, span2;

    if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
	return FALSE;

    span1 = timespan1->time;
    if (timespan1->month != 0) span1 += (timespan1->month * 30);
    span2 = timespan2->time;
    if (timespan2->month != 0) span2 += (timespan2->month * 30);

    return( span1 < span2);
} /* timespan_lt() */

bool
timespan_gt(TimeSpan *timespan1, TimeSpan *timespan2)
{
    double span1, span2;

    if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
	return FALSE;

    span1 = timespan1->time;
    if (timespan1->month != 0) span1 += (timespan1->month * 30);
    span2 = timespan2->time;
    if (timespan2->month != 0) span2 += (timespan2->month * 30);

    return( span1 > span2);
} /* timespan_gt() */

bool
timespan_le(TimeSpan *timespan1, TimeSpan *timespan2)
{
    double span1, span2;

    if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
	return FALSE;

    span1 = timespan1->time;
    if (timespan1->month != 0) span1 += (timespan1->month * 30);
    span2 = timespan2->time;
    if (timespan2->month != 0) span2 += (timespan2->month * 30);

    return( span1 <= span2);
} /* timespan_le() */

bool
timespan_ge(TimeSpan *timespan1, TimeSpan *timespan2)
{
    double span1, span2;

    if (!PointerIsValid(timespan1) || !PointerIsValid(timespan2))
	return FALSE;

    span1 = timespan1->time;
    if (timespan1->month != 0) span1 += (timespan1->month * 30);
    span2 = timespan2->time;
    if (timespan2->month != 0) span2 += (timespan2->month * 30);

    return( span1 >= span2);
} /* timespan_ge() */


/*----------------------------------------------------------
 *  "Arithmetic" operators on date/times.
 *	datetime_foo	returns foo as an object (pointer) that
 *                      can be passed between languages.
 *	datetime_xx	is an internal routine which returns the
 *			actual value.
 *---------------------------------------------------------*/

TimeSpan *datetime_sub(DateTime *datetime1, DateTime *datetime2)
{
    TimeSpan *result;

    DateTime dt1, dt2;

    if (!PointerIsValid(datetime1) || !PointerIsValid(datetime2))
	return NULL;

    dt1 = *datetime1;
    dt2 = *datetime2;

    if (!PointerIsValid(result = PALLOCTYPE(TimeSpan)))
	elog(WARN, "Memory allocation failed, can't subtract dates",NULL);

    if (DATETIME_IS_RELATIVE(dt1)) dt1 = SetDateTime(dt1);
    if (DATETIME_IS_RELATIVE(dt2)) dt2 = SetDateTime(dt2);

    if (DATETIME_IS_INVALID(dt1)
     || DATETIME_IS_INVALID(dt2)) {
	DATETIME_INVALID( result->time);

    } else {
#if FALSE
	result->time = JROUND(dt1 - dt2);
#endif
	result->time = (dt1 - dt2);
    };
    result->month = 0;

    return(result);
} /* datetime_sub() */


DateTime *datetime_add_span(DateTime *datetime, TimeSpan *span)
{
    DateTime *result;

    double date, time;
    int year, mon, mday;

    if ((!PointerIsValid(datetime)) || (!PointerIsValid(span)))
	return NULL;

    if (!PointerIsValid(result = PALLOCTYPE(DateTime)))
	elog(WARN, "Memory allocation failed, can't add dates",NULL);

#ifdef DATEDEBUG
printf( "date_add- add %f to %d %f\n", *datetime, span->month, span->time);
#endif

    *result = *datetime;
    if (DATETIME_IS_RELATIVE(*result)) *result = SetDateTime(*result);

    if (span->month != 0) {
	time = JROUND(modf( (*result/86400), &date)*86400);
	date += date2j(2000,1,1);

	j2date( ((int) date), &year, &mon, &mday);
	mon += span->month;
	if (mon > 12) {
	    year += mon / 12;
	    mon %= 12;
	} else if (mon < 0) {
	    year += mon / 12;
	    mon %= 12;
	    year -= 1;
	    mon += 12;
	};
	*result += ((date2j( year, mon, mday)-date2j(2000,1,1))*86400);
	*result += time;
    };

    *result = JROUND(*result + span->time);

    return(result);
} /* datetime_add_span() */

DateTime *datetime_sub_span(DateTime *datetime, TimeSpan *span)
{
    DateTime *result;
    TimeSpan tspan;

    if (!PointerIsValid(datetime) || !PointerIsValid(span))
	return NULL;

    tspan.month = -span->month;
    tspan.time = -span->time;

    result = datetime_add_span( datetime, &tspan);

    return(result);
} /* datetime_sub_span() */


TimeSpan *timespan_um(TimeSpan *timespan)
{
    TimeSpan *result;

    if (!PointerIsValid(timespan))
	return NULL;

    if (!PointerIsValid(result = PALLOCTYPE(TimeSpan)))
	elog(WARN, "Memory allocation failed, can't subtract dates",NULL);

    result->time = -(timespan->time);
    result->month = -(timespan->month);

    return(result);
} /* datetime_sub() */


TimeSpan *timespan_add(TimeSpan *span1, TimeSpan *span2)
{
    TimeSpan *result;

    if ((!PointerIsValid(span1)) || (!PointerIsValid(span2)))
	return NULL;

    if (!PointerIsValid(result = PALLOCTYPE(TimeSpan)))
	elog(WARN, "Memory allocation failed, can't add timespans",NULL);

    result->month = (span1->month + span2->month);
    result->time = JROUND(span1->time + span2->time);

    return(result);
} /* timespan_add() */

TimeSpan *timespan_sub(TimeSpan *span1, TimeSpan *span2)
{
    TimeSpan *result;

    if ((!PointerIsValid(span1)) || (!PointerIsValid(span2)))
	return NULL;

    if (!PointerIsValid(result = PALLOCTYPE(TimeSpan)))
	elog(WARN, "Memory allocation failed, can't subtract timespans",NULL);

    result->month = (span1->month - span2->month);
    result->time = JROUND(span1->time - span2->time);

    return(result);
} /* timespan_sub() */


/*----------------------------------------------------------
 *  Conversion operators.
 *---------------------------------------------------------*/


float64
datetime_part(text *units, DateTime *datetime)
{
    float64 result;

    DateTime dt;
    int type, val;
    int i;
    char *up, *lp, lowunits[MAXDATELEN];
    double fsec;
    struct tm tt, *tm = &tt;

    if ((!PointerIsValid(units)) || (!PointerIsValid(datetime)))
	return NULL;

    if (!PointerIsValid(result = PALLOCTYPE(float64data)))
	elog(WARN, "Memory allocation failed, can't get date part",NULL);

    up = VARDATA(units);
    lp = lowunits;
    for (i = 0; i < (VARSIZE(units)-VARHDRSZ); i++) *lp++ = tolower( *up++);
    *lp = '\0';

    type = DecodeUnits( 0, lowunits, &val);

#ifdef DATEDEBUG
if (type == IGNORE) strcpy(lowunits, "(unknown)");
printf( "datetime_part- units %s type=%d value=%d\n", lowunits, type, val); 
#endif

    if (DATETIME_NOT_FINITE(*datetime)) {
#if FALSE
	elog(WARN,"Datetime is not finite",NULL);
#endif
	*result = 0;

    } else if (type == UNITS) {

	dt = (DATETIME_IS_RELATIVE(*datetime)? SetDateTime(*datetime): *datetime); 

	if (datetime2tm( dt, tm, &fsec) == 0) {
	    switch (val) {
	    case DTK_TZ:
		*result = CTimeZone;
		break;

	    case DTK_MICROSEC:
		*result = (fsec*1000000);
		break;

	    case DTK_MILLISEC:
		*result = (fsec*1000);
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
		*result = (tm->tm_mon/4)+1;
		break;

	    case DTK_YEAR:
		*result = tm->tm_year;
		break;

	    case DTK_DECADE:
		*result = (tm->tm_year/10)+1;
		break;

	    case DTK_CENTURY:
		*result = (tm->tm_year/100)+1;
		break;

	    case DTK_MILLENIUM:
		*result = (tm->tm_year/1000)+1;
		break;

	    default:
		elog(WARN,"Datetime units %s not yet supported",units);
		*result = 0;
	    };

	} else {
	    elog(NOTICE,"Datetime out of range",NULL);
	    *result = 0;
	};


    } else {
	elog(WARN,"Datetime units %s not recognized",units);
	*result = 0;
    };

    return(result);
} /* datetime_part() */


float64
timespan_part(text *units, TimeSpan *timespan)
{
    float64 result;

    int type, val;
    int i;
    char *up, *lp, lowunits[MAXDATELEN];
    double fsec;
    struct tm tt, *tm = &tt;

    if ((!PointerIsValid(units)) || (!PointerIsValid(timespan)))
	return NULL;

    if (!PointerIsValid(result = PALLOCTYPE(float64data)))
	elog(WARN, "Memory allocation failed, can't get date part",NULL);

    up = VARDATA(units);
    lp = lowunits;
    for (i = 0; i < (VARSIZE(units)-VARHDRSZ); i++) *lp++ = tolower( *up++);
    *lp = '\0';

    type = DecodeUnits( 0, lowunits, &val);

#ifdef DATEDEBUG
if (type == IGNORE) strcpy(lowunits, "(unknown)");
printf( "timespan_part- units %s type=%d value=%d\n", lowunits, type, val); 
#endif

    if (TIMESPAN_IS_INVALID(*timespan)) {
#if FALSE
	elog(WARN,"Timespan is not finite",NULL);
#endif
	*result = 0;

    } else if (type == UNITS) {

	if (timespan2tm(*timespan, tm, &fsec) == 0) {
	    switch (val) {
	    case DTK_MICROSEC:
		*result = (fsec*1000000);
		break;

	    case DTK_MILLISEC:
		*result = (fsec*1000);
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
		*result = (tm->tm_mon/4)+1;
		break;

	    case DTK_YEAR:
		*result = tm->tm_year;
		break;

	    case DTK_DECADE:
		*result = (tm->tm_year/10)+1;
		break;

	    case DTK_CENTURY:
		*result = (tm->tm_year/100)+1;
		break;

	    case DTK_MILLENIUM:
		*result = (tm->tm_year/1000)+1;
		break;

	    default:
		elog(WARN,"Timespan units %s not yet supported",units);
		result = NULL;
	    };

	} else {
	    elog(NOTICE,"Timespan out of range",NULL);
	    *result = 0;
	};


    } else {
	elog(WARN,"Timespan units %s not recognized",units);
	*result = 0;
    };

    return(result);
} /* timespan_part() */


/***************************************************************************** 
 *   PRIVATE ROUTINES                                                        *
 *****************************************************************************/

#if USE_NEW_TIME_CODE
#define DATE_MAXLEN	47
#endif

/* definitions for squeezing values into "value" */
#define ABS_SIGNBIT	0200
#define VALMASK		0177
#define NEG(n)		((n)|ABS_SIGNBIT)
#define SIGNEDCHAR(c)	((c)&ABS_SIGNBIT? -((c)&VALMASK): (c))
#define FROMVAL(tp)	(-SIGNEDCHAR((tp)->value) * 10)	/* uncompress */
#define TOVAL(tp, v)	((tp)->value = ((v) < 0? NEG((-(v))/10): (v)/10))

/*
 * to keep this table reasonably small, we divide the lexval for TZ and DTZ
 * entries by 10 and truncate the text field at MAXTOKLEN characters.
 * the text field is not guaranteed to be NULL-terminated.
 */
static datetkn datetktbl[] = {
/*	text		token	lexval */
{	EARLY,		RESERV,	DTK_EARLY},	/* "-infinity" reserved for "early time" */
{	"acsst",	DTZ,	63},		/* Cent. Australia */
{	"acst",		TZ,	57},		/* Cent. Australia */
{	DA_D,		ADBC,	AD},		/* "ad" for years >= 0 */
{	"abstime",	IGNORE,	0},		/* "abstime" for pre-v6.1 "Invalid Abstime" */
{	"adt",		DTZ,	NEG(18)},	/* Atlantic Daylight Time */
{	"aesst",	DTZ,	66},		/* E. Australia */
{	"aest",		TZ,	60},		/* Australia Eastern Std Time */
{	"ahst",		TZ,	60},		/* Alaska-Hawaii Std Time */
{	"allballs",	RESERV,	DTK_ZULU},	/* 00:00:00 */
{	"am",		AMPM,	AM},
{	"apr",		MONTH,	4},
{	"april",	MONTH,	4},
{	"ast",		TZ,	NEG(24)},	/* Atlantic Std Time (Canada) */
{	"at",		IGNORE,	0},		/* "at" (throwaway) */
{	"aug",		MONTH,	8},
{	"august",	MONTH,	8},
{	"awsst",	DTZ,	54},		/* W. Australia */
{	"awst",		TZ,	48},		/* W. Australia */
{	DB_C,		ADBC,	BC},		/* "bc" for years < 0 */
{	"bst",		TZ,	6},		/* British Summer Time */
{	"bt",		TZ,	18},		/* Baghdad Time */
{	"cadt",		DTZ,	63},		/* Central Australian DST */
{	"cast",		TZ,	57},		/* Central Australian ST */
{	"cat",		TZ,	NEG(60)},	/* Central Alaska Time */
{	"cct",		TZ,	48},		/* China Coast */
{	"cdt",		DTZ,	NEG(30)},	/* Central Daylight Time */
{	"cet",		TZ,	6},		/* Central European Time */
{	"cetdst",	DTZ,	12},		/* Central European Dayl.Time */
{	"cst",		TZ,	NEG(36)},	/* Central Standard Time */
{	DCURRENT,	RESERV,	DTK_CURRENT},	/* "current" is always now */
{	"dec",		MONTH,	12},
{	"december",	MONTH,	12},
{	"dnt",		TZ,	6},		/* Dansk Normal Tid */
{	"dst",		IGNORE,	0},
{	"east",		TZ,	NEG(60)},	/* East Australian Std Time */
{	"edt",		DTZ,	NEG(24)},	/* Eastern Daylight Time */
{	"eet",		TZ,	12},		/* East. Europe, USSR Zone 1 */
{	"eetdst",	DTZ,	18},		/* Eastern Europe */
{	EPOCH,		RESERV,	DTK_EPOCH},	/* "epoch" reserved for system epoch time */
{	"est",		TZ,	NEG(30)},	/* Eastern Standard Time */
{	"feb",		MONTH,	2},
{	"february",	MONTH,	2},
{	"fri",		DOW,	5},
{	"friday",	DOW,	5},
{	"fst",		TZ,	6},		/* French Summer Time */
{	"fwt",		DTZ,	12},		/* French Winter Time  */
{	"gmt",		TZ,	0},		/* Greenwish Mean Time */
{	"gst",		TZ,	60},		/* Guam Std Time, USSR Zone 9 */
{	"hdt",		DTZ,	NEG(54)},	/* Hawaii/Alaska */
{	"hmt",		DTZ,	18},		/* Hellas ? ? */
{	"hst",		TZ,	NEG(60)},	/* Hawaii Std Time */
{	"idle",		TZ,	72},		/* Intl. Date Line, East */
{	"idlw",		TZ,	NEG(72)},	/* Intl. Date Line, West */
{	LATE,		RESERV,	DTK_LATE},	/* "infinity" reserved for "late time" */
{	INVALID,	RESERV,	DTK_INVALID},	/* "invalid" reserved for invalid time */
{	"ist",		TZ,	12},		/* Israel */
{	"it",		TZ,	22},		/* Iran Time */
{	"jan",		MONTH,	1},
{	"january",	MONTH,	1},
{	"jst",		TZ,	54},		/* Japan Std Time,USSR Zone 8 */
{	"jt",		TZ,	45},		/* Java Time */
{	"jul",		MONTH,	7},
{	"july",		MONTH,	7},
{	"jun",		MONTH,	6},
{	"june",		MONTH,	6},
{	"kst",		TZ,	54},		/* Korea Standard Time */
{	"ligt",		TZ,	60},		/* From Melbourne, Australia */
{	"mar",		MONTH,	3},
{	"march",	MONTH,	3},
{	"may",		MONTH,	5},
{	"mdt",		DTZ,	NEG(36)},	/* Mountain Daylight Time */
{	"mest",		DTZ,	12},		/* Middle Europe Summer Time */
{	"met",		TZ,	6},		/* Middle Europe Time */
{	"metdst",	DTZ,	12},		/* Middle Europe Daylight Time*/
{	"mewt",		TZ,	6},		/* Middle Europe Winter Time */
{	"mez",		TZ,	6},		/* Middle Europe Zone */
{	"mon",		DOW,	1},
{	"monday",	DOW,	1},
{	"mst",		TZ,	NEG(42)},	/* Mountain Standard Time */
{	"mt",		TZ,	51},		/* Moluccas Time */
{	"ndt",		DTZ,	NEG(15)},	/* Nfld. Daylight Time */
{	"nft",		TZ,	NEG(21)},	/* Newfoundland Standard Time */
{	"nor",		TZ,	6},		/* Norway Standard Time */
{	"nov",		MONTH,	11},
{	"november",	MONTH,	11},
{	NOW,		RESERV,	DTK_NOW},	/* current transaction time */
{	"nst",		TZ,	NEG(21)},	/* Nfld. Standard Time */
{	"nt",		TZ,	NEG(66)},	/* Nome Time */
{	"nzdt",		DTZ,	78},		/* New Zealand Daylight Time */
{	"nzst",		TZ,	72},		/* New Zealand Standard Time */
{	"nzt",		TZ,	72},		/* New Zealand Time */
{	"oct",		MONTH,	10},
{	"october",	MONTH,	10},
{	"on",		IGNORE,	0},		/* "on" (throwaway) */
{	"pdt",		DTZ,	NEG(42)},	/* Pacific Daylight Time */
{	"pm",		AMPM,	PM},
{	"pst",		TZ,	NEG(48)},	/* Pacific Standard Time */
{	"sadt",		DTZ,	63},		/* S. Australian Dayl. Time */
{	"sast",		TZ,	57},		/* South Australian Std Time */
{	"sat",		DOW,	6},
{	"saturday",	DOW,	6},
{	"sep",		MONTH,	9},
{	"sept",		MONTH,	9},
{	"september",	MONTH,	9},
{	"set",		TZ,	NEG(6)},	/* Seychelles Time ?? */
{	"sst",		DTZ,	12},		/* Swedish Summer Time */
{	"sun",		DOW,	0},
{	"sunday",	DOW,	0},
{	"swt",		TZ,	6},		/* Swedish Winter Time  */
{	"thu",		DOW,	4},
{	"thur",		DOW,	4},
{	"thurs",	DOW,	4},
{	"thursday",	DOW,	4},
{	TODAY,		RESERV,	DTK_TODAY},	/* midnight */
{	TOMORROW,	RESERV,	DTK_TOMORROW},	/* tomorrow midnight */
{	"tue",		DOW,	2},
{	"tues",		DOW,	2},
{	"tuesday",	DOW,	2},
{	"undefined",	RESERV,	DTK_INVALID},	/* "undefined" pre-v6.1 invalid time */
{	"ut",		TZ,	0},
{	"utc",		TZ,	0},
{	"wadt",		DTZ,	48},		/* West Australian DST */
{	"wast",		TZ,	42},		/* West Australian Std Time */
{	"wat",		TZ,	NEG(6)},	/* West Africa Time */
{	"wdt",		DTZ,	54},		/* West Australian DST */
{	"wed",		DOW,	3},
{	"wednesday",	DOW,	3},
{	"weds",		DOW,	3},
{	"wet",		TZ,	0},		/* Western Europe */
{	"wetdst",	DTZ,	6},		/* Western Europe */
{	"wst",		TZ,	48},		/* West Australian Std Time */
{	"ydt",		DTZ,	NEG(48)},	/* Yukon Daylight Time */
{	YESTERDAY,	RESERV,	DTK_YESTERDAY},	/* yesterday midnight */
{	"yst",		TZ,	NEG(54)},	/* Yukon Standard Time */
{	"zp4",		TZ,	NEG(24)},	/* GMT +4  hours. */
{	"zp5",		TZ,	NEG(30)},	/* GMT +5  hours. */
{	"zp6",		TZ,	NEG(36)},	/* GMT +6  hours. */
{	ZULU,		RESERV,	DTK_ZULU},	/* 00:00:00 */
};

static unsigned int szdatetktbl = sizeof datetktbl / sizeof datetktbl[0];

static datetkn deltatktbl[] = {
/*	text		token	lexval */
{	"@",		IGNORE,	0},		/* postgres relative time prefix */
{	DAGO,		AGO,	0},		/* "ago" indicates negative time offset */
{	"c",		UNITS,	DTK_CENTURY},	/* "century" relative time units */
{	"cent",		UNITS,	DTK_CENTURY},	/* "century" relative time units */
{	"centuries",	UNITS,	DTK_CENTURY},	/* "centuries" relative time units */
{	DCENTURY,	UNITS,	DTK_CENTURY},	/* "century" relative time units */
{	"d",		UNITS,	DTK_DAY},	/* "day" relative time units */
{	DDAY,		UNITS,	DTK_DAY},	/* "day" relative time units */
{	"days",		UNITS,	DTK_DAY},	/* "days" relative time units */
{	"dec",		UNITS,	DTK_DECADE},	/* "decade" relative time units */
{	"decs",		UNITS,	DTK_DECADE},	/* "decades" relative time units */
{	DDECADE,	UNITS,	DTK_DECADE},	/* "decade" relative time units */
{	"decades",	UNITS,	DTK_DECADE},	/* "decades" relative time units */
{	"h",		UNITS,	DTK_HOUR},	/* "hour" relative time units */
{	DHOUR,		UNITS,	DTK_HOUR},	/* "hour" relative time units */
{	"hours",	UNITS,	DTK_HOUR},	/* "hours" relative time units */
{	"hr",		UNITS,	DTK_HOUR},	/* "hour" relative time units */
{	"hrs",		UNITS,	DTK_HOUR},	/* "hours" relative time units */
{	INVALID,	RESERV,	DTK_INVALID},	/* "invalid" reserved for invalid time */
{	"m",		UNITS,	DTK_MINUTE},	/* "minute" relative time units */
{	"microsecon",	UNITS,	DTK_MILLISEC},	/* "microsecond" relative time units */
{	"mil",		UNITS,	DTK_MILLENIUM},	/* "millenium" relative time units */
{	"mils",		UNITS,	DTK_MILLENIUM},	/* "millenia" relative time units */
{	"millenia",	UNITS,	DTK_MILLENIUM},	/* "millenia" relative time units */
{	DMILLENIUM,	UNITS,	DTK_MILLENIUM},	/* "millenium" relative time units */
{	"millisecon",	UNITS,	DTK_MILLISEC},	/* "millisecond" relative time units */
{	"min",		UNITS,	DTK_MINUTE},	/* "minute" relative time units */
{	"mins",		UNITS,	DTK_MINUTE},	/* "minutes" relative time units */
{	"mins",		UNITS,	DTK_MINUTE},	/* "minutes" relative time units */
{	DMINUTE,	UNITS,	DTK_MINUTE},	/* "minute" relative time units */
{	"minutes",	UNITS,	DTK_MINUTE},	/* "minutes" relative time units */
{	"mon",		UNITS,	DTK_MONTH},	/* "months" relative time units */
{	"mons",		UNITS,	DTK_MONTH},	/* "months" relative time units */
{	DMONTH,		UNITS,	DTK_MONTH},	/* "month" relative time units */
{	"months",	UNITS,	DTK_MONTH},	/* "months" relative time units */
{	"ms",		UNITS,	DTK_MILLISEC},	/* "millisecond" relative time units */
{	"msec",		UNITS,	DTK_MILLISEC},	/* "millisecond" relative time units */
{	DMILLISEC,	UNITS,	DTK_MILLISEC},	/* "millisecond" relative time units */
{	"mseconds",	UNITS,	DTK_MILLISEC},	/* "milliseconds" relative time units */
{	"msecs",	UNITS,	DTK_MILLISEC},	/* "milliseconds" relative time units */
{	"qtr",		UNITS,	DTK_QUARTER},	/* "quarter" relative time units */
{	DQUARTER,	UNITS,	DTK_QUARTER},	/* "quarter" relative time units */
{	"reltime",	IGNORE,	0},		/* "reltime" for pre-v6.1 "Undefined Reltime" */
{	"s",		UNITS,	DTK_SECOND},	/* "second" relative time units */
{	"sec",		UNITS,	DTK_SECOND},	/* "second" relative time units */
{	DSECOND,	UNITS,	DTK_SECOND},	/* "second" relative time units */
{	"seconds",	UNITS,	DTK_SECOND},	/* "seconds" relative time units */
{	"secs",		UNITS,	DTK_SECOND},	/* "seconds" relative time units */
{	DTIMEZONE,	UNITS,	DTK_TZ},	/* "timezone" time offset */
{	"tz",		UNITS,	DTK_TZ},	/* "timezone" time offset */
{	"undefined",	RESERV,	DTK_INVALID},	/* "undefined" pre-v6.1 invalid time */
{	"us",		UNITS,	DTK_MICROSEC},	/* "microsecond" relative time units */
{	"usec",		UNITS,	DTK_MICROSEC},	/* "microsecond" relative time units */
{	DMICROSEC,	UNITS,	DTK_MICROSEC},	/* "microsecond" relative time units */
{	"useconds",	UNITS,	DTK_MICROSEC},	/* "microseconds" relative time units */
{	"usecs",	UNITS,	DTK_MICROSEC},	/* "microseconds" relative time units */
{	"w",		UNITS,	DTK_WEEK},	/* "week" relative time units */
{	DWEEK,		UNITS,	DTK_WEEK},	/* "week" relative time units */
{	"weeks",	UNITS,	DTK_WEEK},	/* "weeks" relative time units */
{	"y",		UNITS,	DTK_YEAR},	/* "year" relative time units */
{	DYEAR,		UNITS,	DTK_YEAR},	/* "year" relative time units */
{	"years",	UNITS,	DTK_YEAR},	/* "years" relative time units */
{	"yr",		UNITS,	DTK_YEAR},	/* "year" relative time units */
{	"yrs",		UNITS,	DTK_YEAR},	/* "years" relative time units */
};

static unsigned int szdeltatktbl = sizeof deltatktbl / sizeof deltatktbl[0];

#if USE_DATE_CACHE
datetkn *datecache[MAXDATEFIELDS] = {NULL};

datetkn *deltacache[MAXDATEFIELDS] = {NULL};
#endif


/*
 * Calendar time to Julian date conversions.
 * Julian date is commonly used in astronomical applications,
 *  since it is numerically accurate and computationally simple.
 * The algorithms here will accurately convert between Julian day
 *  and calendar date for all non-negative Julian days
 *  (i.e. from Nov 23, -4713 on).
 *
 * Ref: Explanatory Supplement to the Astronomical Almanac, 1992.
 *  University Science Books, 20 Edgehill Rd. Mill Valley CA 94941.
 *
 * These routines will be used by other date/time packages - tgl 97/02/25
 */

#define USE_FLIEGEL 1

int
date2j(int y, int m, int d)
{
#if USE_FLIEGEL
    int m12 = (m-14)/12;

    return((1461*(y+4800+m12))/4 + (367*(m-2-12*(m12)))/12
     - (3*((y+4900+m12)/100))/4 + d - 32075);

#else
    int c, ya;

    if (m > 2) {
	m -= 3;
    } else {
	m += 9;
	y--;
    };
    c = y/100;
    ya = y - 100*c;
    return((146097*c)/4+(1461*ya)/4+(153*m+2)/5+d+1721119);
#endif
} /* date2j() */

void j2date( int jd, int *year, int *month, int *day)
{
    int j, y, m, d;

#if USE_FLIEGEL
    int i, l, n;

    l = jd + 68569;
    n = (4*l)/146097;
    l -= (146097*n+3)/4;
    i = (4000*(l+1))/1461001;
    l += 31 - (1461*i)/4;
    j = (80*l)/2447;
    d = l - (2447*j)/80;
    l = j/11;
    m = (j+2) - (12*l);
    y = 100*(n-49)+i+l;

#else
    j = jd - 1721119;
    y = (4*j-1)/146097;
    j = 4*j-1-146097*y;
    d = j/4;
    j = (4*d+3)/1461;
    d = 4*d+3-1461*j;
    d = (d+4)/4;
    m = (5*d-3)/153;
    d = 5*d-3-153*m;
    d = (d+5)/5;
    y = 100*y+j;
    if (m < 10) {
	m += 3;
    } else {
	m -= 9;
	y++;
    };
#endif

    *year = y;
    *month = m;
    *day = d;
    return;
} /* j2date() */

int j2day( int date)
{
    int day;

    day = (date+1) % 7;

    return(day);
} /* j2day() */

int
datetime2tm( DateTime dt, struct tm *tm, double *fsec)
{
    double date, time, sec;

    time = (modf( dt2local( dt, CTimeZone)/86400, &date)*86400);
    date += date2j(2000,1,1);
    if (time < 0) {
	    time += 86400;
	    date -= 1;
    };

    /* Julian day routine does not work for negative Julian days */
    if (date < 0) return -1;

#ifdef DATEDEBUG
printf( "datetime2tm- date is %f (%f %f)\n", dt, date, time);
#endif

    j2date((int) date, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
    dt2time( time, &tm->tm_hour, &tm->tm_min, &sec);

#ifdef DATEDEBUG
printf( "datetime2tm- date is %d.%02d.%02d\n", tm->tm_year, tm->tm_mon, tm->tm_mday);
printf( "datetime2tm- time is %02d:%02d:%2.2f\n", tm->tm_hour, tm->tm_min, sec);
#endif

    *fsec = modf(JROUND(sec),&sec);
    tm->tm_sec = sec;

#ifdef DATEDEBUG
printf( "datetime2tm- time is %02d:%02d:%02d %.7f\n", tm->tm_hour, tm->tm_min, tm->tm_sec, *fsec);
#endif

    tm->tm_isdst = -1;

#ifdef DATEDEBUG
printf( "datetime2tm- timezone is %s; offset is %d; daylight is %d\n",
 CTZName, CTimeZone, CDayLight);
#endif

    return 0;
} /* datetime2tm() */

/* tm2datetime()
 * Convert a tm structure to a datetime data type.
 */
DateTime
tm2datetime( struct tm *tm, double fsec, int tzp) {

    DateTime result;
    double date, time;

    /* Julian day routines are not correct for negative Julian days */
    if ((date = date2j(tm->tm_year,tm->tm_mon,tm->tm_mday)) < 0)
	return(DT_INVALID);

    date -= date2j(2000,1,1);
    time = time2t(tm->tm_hour,tm->tm_min,(tm->tm_sec + fsec));
    result = (date*86400+time);
#ifdef DATEDEBUG
printf( "tm2datetime- date is %f (%f %f %d)\n", result, date, time, (((tm->tm_hour*60)+tm->tm_min)*60+tm->tm_sec));
printf( "tm2datetime- time is %f %02d:%02d:%02d %f\n", time, tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);
#endif
    if (tzp != 0) result = dt2local(result, -tzp);

    return(result);
} /* tm2datetime() */


int
timespan2tm(TimeSpan span, struct tm *tm, float8 *fsec)
{
    double time, iunit, funit;

    if (span.month != 0) {
	tm->tm_year = span.month / 12;
	tm->tm_mon = span.month % 12;

    } else {
	tm->tm_year = 0;
	tm->tm_mon = 0;
    };

#if FALSE
    time = JROUND(span.time);
#endif
    time = span.time;

    funit = modf( (time / 86400), &iunit);
    tm->tm_mday = iunit;
    if (tm->tm_mday != 0) time -= rint(tm->tm_mday * 86400);

    funit = modf( (time / 3600), &iunit);
    tm->tm_hour = iunit;
    if (tm->tm_hour != 0) time -= rint(tm->tm_hour * 3600e0);
    funit = modf( (time / 60), &iunit);
    tm->tm_min = iunit;
    if (tm->tm_min != 0) time -= rint(tm->tm_min * 60e0);
    funit = modf( time, &iunit);
    tm->tm_sec = iunit;
    if (tm->tm_sec != 0) time -= tm->tm_sec;
    *fsec = time;

#ifdef DATEDEBUG
printf( "timespan2tm- %d %f = %04d-%02d-%02d %02d:%02d:%02d %.2f\n", span.month, span.time,
 tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, *fsec);
#endif

    return 0;
} /* timespan2tm() */

int
tm2timespan( struct tm *tm, double fsec, TimeSpan *span)
{
    span->month = ((tm->tm_year*12)+tm->tm_mon);
    span->time = ((((((tm->tm_mday*24)+tm->tm_hour)*60)+tm->tm_min)*60)+tm->tm_sec);
    span->time = JROUND(span->time + fsec);

#ifdef DATEDEBUG
printf( "tm2timespan- %d %f = %04d-%02d-%02d %02d:%02d:%02d %.2f\n", span->month, span->time,
 tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);
#endif

    return 1;
} /* tm2timespan() */


DateTime dt2local(DateTime dt, int timezone)
{
    dt -= timezone;
    dt = JROUND(dt);
    return(dt);
} /* dt2local() */

double time2t(const int hour, const int min, const double sec)
{
    return((((hour*60)+min)*60)+sec);
} /* time2t() */

void dt2time(DateTime jd, int *hour, int *min, double *sec)
{
    double time;

    time = jd;

    *hour = (time/3600);
    time -= ((*hour)*3600);
    *min = (time/60);
    time -= ((*min)*60);
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
ParseDateTime( char *timestr, char *lowstr,
  char *field[], int ftype[], int maxfields, int *numfields)
{
    int nf = 0;
    char *cp = timestr;
    char *lp = lowstr;

#ifdef DATEDEBUG
printf( "ParseDateTime- input string is %s\n", timestr);
#endif
    /* outer loop through fields */
    while (*cp != '\0') {
	field[nf] = lp;

	/* leading digit? then date or time */
	if (isdigit(*cp)) {
	    *lp++ = *cp++;
	    while (isdigit(*cp)) *lp++ = *cp++;
	    /* time field? */
	    if (*cp == ':') {
		ftype[nf] = DTK_TIME;
		while (isdigit(*cp) || (*cp == ':') || (*cp == '.'))
		    *lp++ = *cp++;

	    /* date field? allow embedded text month */
	    } else if ((*cp == '-') || (*cp == '/') || (*cp == '.')) {
		ftype[nf] = DTK_DATE;
		while (isalnum(*cp) || (*cp == '-') || (*cp == '/') || (*cp == '.'))
		    *lp++ = tolower(*cp++);

	    /* otherwise, number only and will determine year, month, or day later */
	    } else {
		ftype[nf] = DTK_NUMBER;
	    };

	/* text? then date string, month, day of week, special, or timezone */
	} else if (isalpha(*cp)) {
	    ftype[nf] = DTK_STRING;
	    *lp++ = tolower(*cp++);
	    while (isalpha(*cp)) *lp++ = tolower(*cp++);

	    /* full date string with leading text month? */
	    if ((*cp == '-') || (*cp == '/') || (*cp == '.')) {
		ftype[nf] = DTK_DATE;
		while (isdigit(*cp) || (*cp == '-') || (*cp == '/') || (*cp == '.'))
		    *lp++ = tolower(*cp++);
	    };

	/* skip leading spaces */
	} else if (isspace(*cp)) {
	    cp++;
	    continue;

	/* sign? then special or numeric timezone */
	} else if ((*cp == '+') || (*cp == '-')) {
	    *lp++ = *cp++;
	    /* soak up leading whitespace */
	    while (isspace(*cp)) cp++;
	    /* numeric timezone? */
	    if (isdigit(*cp)) {
		ftype[nf] = DTK_TZ;
		*lp++ = *cp++;
		while (isdigit(*cp) || (*cp == ':')) *lp++ = *cp++;

	    /* special? */
	    } else if (isalpha(*cp)) {
		ftype[nf] = DTK_SPECIAL;
		*lp++ = tolower(*cp++);
		while (isalpha(*cp)) *lp++ = tolower(*cp++);

	    /* otherwise something wrong... */
	    } else {
		return -1;
	    };

	/* ignore punctuation but use as delimiter */
	} else if (ispunct(*cp)) {
	    cp++;
	    continue;

	} else {
	    return -1;
	};

	/* force in a delimiter */
	*lp++ = '\0';
	nf++;
	if (nf > MAXDATEFIELDS) {
	    return -1;
	};
#ifdef DATEDEBUG
printf( "ParseDateTime- set field[%d] to %s type %d\n", (nf-1), field[nf-1], ftype[nf-1]);
#endif
    };

    *numfields = nf;

    return 0;
} /* ParseDateTime() */


/* DecodeDateTime()
 * Interpret previously parsed fields for general date and time.
 * Return 0 if full date, 1 if only time, and -1 if problems.
 *	External format(s):
 *		"<weekday> <month>-<day>-<year> <hour>:<minute>:<second>"
 *		"Fri Feb-7-1997 15:23:27"
 *		"Feb-7-1997 15:23:27"
 *		"2-7-1997 15:23:27"
 *		"1997-2-7 15:23:27"
 *		"1997.038 15:23:27"	(day of year 1-366)
 *	Also supports input in compact time:
 *		"970207 152327"
 *		"97038 152327"
 */
int
DecodeDateTime( char *field[], int ftype[], int nf,
 int *dtype, struct tm *tm, double *fsec, int *tzp)
{
    int fmask = 0, tmask, type;
    int i;
    int flen, val;
    int mer = HR24;
    int bc = FALSE;

    *dtype = DTK_DATE;
    tm->tm_hour = 0;
    tm->tm_min = 0;
    tm->tm_sec = 0;
    *fsec = 0;
    tm->tm_isdst = -1;	/* don't know daylight savings time status apriori */
    if (tzp != NULL) *tzp = CTimeZone;

    for (i = 0; i < nf; i++) {
#ifdef DATEDEBUG
printf( "DecodeDateTime- field[%d] is %s (type %d)\n", i, field[i], ftype[i]);
#endif
	switch (ftype[i]) {
	case DTK_DATE:
	    if (DecodeDate(field[i], fmask, &tmask, tm) != 0) return -1;
	    break;

	case DTK_TIME:
	    if (DecodeTime(field[i], fmask, &tmask, tm, fsec) != 0) return -1;
	    break;

	case DTK_TZ:
	    if (tzp == NULL) return -1;
	    if (DecodeTimezone( field[i], tzp) != 0) return -1;
	    tmask = DTK_M(TZ);
	    break;

	case DTK_NUMBER:
	    flen = strlen(field[i]);

	    if (flen > 4) {
		if (DecodeNumberField( flen, field[i], fmask, &tmask, tm, fsec) != 0)
		    return -1;

	    } else {
		if (DecodeNumber( flen, field[i], fmask, &tmask, tm, fsec) != 0)
		    return -1;
	    };
	    break;

	case DTK_STRING:
	case DTK_SPECIAL:
	    type = DecodeSpecial( i, field[i], &val);
#ifdef DATEDEBUG
printf( "DecodeDateTime- special field[%d] %s type=%d value=%d\n", i, field[i], type, val); 
#endif
	    if (type == IGNORE) continue;

	    tmask = DTK_M(type);
	    switch (type) {
	    case RESERV:
#ifdef DATEDEBUG
printf( "DecodeDateTime- RESERV field %s value is %d\n", field[i], val); 
#endif
		switch (val) {
		case DTK_NOW:
		    tmask = (DTK_DATE_M | DTK_TIME_M);
		    *dtype = DTK_DATE;
		    GetCurrentTime(tm);
		    break;

		case DTK_YESTERDAY:
		    tmask = DTK_DATE_M;
		    *dtype = DTK_DATE;
		    GetCurrentTime(tm);
		    j2date( (date2j( tm->tm_year, tm->tm_mon, tm->tm_mday)-1),
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
		    j2date( (date2j( tm->tm_year, tm->tm_mon, tm->tm_mday)+1),
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
		    *tzp = 0;
		    break;

		default:
		    *dtype = val;
		};

		break;

	    case MONTH:
#ifdef DATEDEBUG
printf( "DecodeDateTime- month field %s value is %d\n", field[i], val); 
#endif
		tm->tm_mon = val;
		break;

	    case DTZ:
		tm->tm_isdst = 0;
		/* FALLTHROUGH! */
	    case TZ:
		if (tzp == NULL) return -1;
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
	    };
	    break;

	default:
	    return -1;
	};

#ifdef DATEDEBUG
printf( "DecodeDateTime- field[%d] %s (%08x/%08x) value is %d\n",
 i, field[i], fmask, tmask, val); 
#endif

	if (tmask & fmask) return -1;
	fmask |= tmask;
    };

    /* there is no year zero in AD/BC notation; i.e. "1 BC" == year 0 */
    if (bc) tm->tm_year = -(tm->tm_year-1);

    if ((mer != HR24) && (tm->tm_hour > 12))
	return -1;
    if (mer == PM) tm->tm_hour += 12;

#ifdef DATEDEBUG
printf( "DecodeDateTime- mask %08x (%08x)", fmask, DTK_DATE_M);
printf( " set y%04d m%02d d%02d", tm->tm_year, tm->tm_mon, tm->tm_mday);
printf( " %02d:%02d:%02d\n", tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif

    if ((*dtype == DTK_DATE) && ((fmask & DTK_DATE_M) != DTK_DATE_M))
	return(((fmask & DTK_TIME_M) == DTK_TIME_M)? 1: -1);

    return 0;
} /* DecodeDateTime() */


/* DecodeTimeOnly()
 * Interpret parsed string as time fields only.
 */
int
DecodeTimeOnly( char *field[], int ftype[], int nf, int *dtype, struct tm *tm, double *fsec)
{
    int fmask, tmask, type;
    int i;
    int flen, val;
    int mer = HR24;

    *dtype = DTK_TIME;
    tm->tm_hour = 0;
    tm->tm_min = 0;
    tm->tm_sec = 0;
    tm->tm_isdst = -1;	/* don't know daylight savings time status apriori */

    fmask = DTK_DATE_M;

    for (i = 0; i < nf; i++) {
#ifdef DATEDEBUG
printf( "DecodeTimeOnly- field[%d] is %s (type %d)\n", i, field[i], ftype[i]);
#endif
	switch (ftype[i]) {
	case DTK_TIME:
	    if (DecodeTime(field[i], fmask, &tmask, tm, fsec) != 0) return -1;
	    break;

	case DTK_NUMBER:
	    flen = strlen(field[i]);

	    if (DecodeNumberField( flen, field[i], fmask, &tmask, tm, fsec) != 0)
		return -1;
	    break;

	case DTK_STRING:
	case DTK_SPECIAL:
	    type = DecodeSpecial( i, field[i], &val);
#ifdef DATEDEBUG
printf( "DecodeTimeOnly- special field[%d] %s type=%d value=%d\n", i, field[i], type, val); 
#endif
	    if (type == IGNORE) continue;

	    tmask = DTK_M(type);
	    switch (type) {
	    case RESERV:
#ifdef DATEDEBUG
printf( "DecodeTimeOnly- RESERV field %s value is %d\n", field[i], val); 
#endif
		switch (val) {
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
		    tm->tm_isdst = -1;
		    break;

		default:
		    return -1;
		};

		break;

	    case IGNORE:
		break;

	    case AMPM:
		mer = val;
		break;

	    default:
		return -1;
	    };
	    break;

	default:
	    return -1;
	};

	if (tmask & fmask) return -1;
	fmask |= tmask;

#ifdef DATEDEBUG
printf( "DecodeTimeOnly- field[%d] %s value is %d\n", i, field[i], val); 
#endif
    };

#ifdef DATEDEBUG
printf( "DecodeTimeOnly- mask %08x (%08x)", fmask, DTK_TIME_M);
printf( " %02d:%02d:%02d (%f)\n", tm->tm_hour, tm->tm_min, tm->tm_sec, *fsec);
#endif

    if ((mer != HR24) && (tm->tm_hour > 12))
	return -1;
    if (mer == PM) tm->tm_hour += 12;

    if ((fmask & DTK_TIME_M) != DTK_TIME_M)
	return -1;

    return 0;
} /* DecodeTimeOnly() */


/* DecodeDate()
 * Decode date string which includes delimiters.
 * Insist on a complete set of fields.
 */
int
DecodeDate(char *str, int fmask, int *tmask, struct tm *tm)
{
    double fsec;

    int nf = 0;
    int i, len;
    int type, val, dmask = 0;
    char *field[MAXDATEFIELDS];

    /* parse this string... */
    while ((*str != '\0') && (nf < MAXDATEFIELDS)) {
	/* skip field separators */
	while (! isalnum(*str)) str++;

	field[nf] = str;
	if (isdigit(*str)) {
	    while (isdigit(*str)) str++;
	} else if (isalpha(*str)) {
	    while (isalpha(*str)) str++;
	};

	if (*str != '\0') *str++ = '\0';
	nf++;
    };

    /* don't allow too many fields */
    if (nf > 3) return -1;

    *tmask = 0;

    for (i = 0; i < nf; i++) {
	str = field[i];
	len = strlen(str);

	if (len <= 0)
	    return -1;

	if (isdigit(*str)) {
	    if (DecodeNumber( len, str, fmask, &dmask, tm, &fsec) != 0)
		return -1;

	} else if (isalpha(*str)) {
	    type = DecodeSpecial( i, field[i], &val);
	    if (type == IGNORE) continue;

	    dmask = DTK_M(type);
	    switch (type) {
	    case YEAR:
#ifdef DATEDEBUG
printf( "DecodeDate- year field %s value is %d\n", field[i], val); 
#endif
		tm->tm_mon = val;
		break;

	    case MONTH:
#ifdef DATEDEBUG
printf( "DecodeDate- month field %s value is %d\n", field[i], val); 
#endif
		tm->tm_mon = val;
		break;

	    case DAY:
#ifdef DATEDEBUG
printf( "DecodeDate- month field %s value is %d\n", field[i], val); 
#endif
		tm->tm_mon = val;
		break;

	    default:
#ifdef DATEDEBUG
printf( "DecodeDate- illegal field %s value is %d\n", field[i], val); 
#endif
		return -1;
	    };
	};

	if (fmask & dmask) return -1;

	fmask |= dmask;
	*tmask |= dmask;
    };

    return 0;
} /* DecodeDate() */


/* DecodeTime()
 * Decode time string which includes delimiters.
 */
int
DecodeTime(char *str, int fmask, int *tmask, struct tm *tm, double *fsec)
{
    char *cp;

    *tmask = DTK_TIME_M;

    tm->tm_hour = strtol( str, &cp, 10);
    if (*cp != ':') return -1;
    str = cp+1;
    tm->tm_min = strtol( str, &cp, 10);
    if (*cp == '\0') {
	tm->tm_sec = 0;
	*fsec = 0;

    } else if (*cp != ':') {
	return -1;

    } else {
	str = cp+1;
	tm->tm_sec = strtol( str, &cp, 10);
	if (*cp == '\0') {
	    *fsec = 0;
	} else if (*cp == '.') {
	    str = cp;
	    *fsec = strtod( str, &cp);
	    if (cp == str) return -1;
	} else {
	    return -1;
	};
    };

    return 0;
} /* DecodeTime() */


/* DecodeNumber()
 * Interpret numeric field as a date value in context.
 */
int
DecodeNumber( int flen, char *str, int fmask, int *tmask, struct tm *tm, double *fsec)
{
    int val;
    char *cp;

    *tmask = 0;

    val = strtol( str, &cp, 10);
    if (cp == str) return -1;
    if (*cp == '.') {
	*fsec = strtod( cp, &cp);
	if (*cp != '\0') return -1;
    };

#ifdef DATEDEBUG
printf( "DecodeNumber- %s is %d fmask=%08x tmask=%08x\n", str, val, fmask, *tmask);
#endif

    /* enough digits to be unequivocal year? */
    if (flen == 4) {
#ifdef DATEDEBUG
printf( "DecodeNumber- match %d (%s) as year\n", val, str);
#endif
	*tmask = DTK_M(YEAR);
	tm->tm_year = val;

    /* special case day of year? */
    } else if ((flen == 3) && (fmask & DTK_M(YEAR))
      && ((val >= 1) && (val <= 366))) {
	*tmask = (DTK_M(DOY) | DTK_M(MONTH) | DTK_M(DAY));
	tm->tm_yday = val;
	j2date((date2j(tm->tm_year,1,1)+tm->tm_yday-1),
	  &tm->tm_year,&tm->tm_mon,&tm->tm_mday);

    /* already have year? then could be month */
    } else if ((fmask && DTK_M(YEAR)) && (! (fmask & DTK_M(MONTH)))
      && ((val >= 1) && (val <= 12))) {
#ifdef DATEDEBUG
printf( "DecodeNumber- match %d (%s) as month\n", val, str);
#endif
	*tmask = DTK_M(MONTH);
	tm->tm_mon = val;

    /* no year and EuroDates enabled? then could be day */
#if USE_EURODATES
    } else if ((EuroDates || (fmask & DTK_M(MONTH)))
#else
    } else if ((fmask & DTK_M(MONTH))
#endif
      && (! ((fmask & DTK_M(YEAR)) && (fmask & DTK_M(DAY))))
      && ((val >= 1) && (val <= 31))) {
#ifdef DATEDEBUG
printf( "DecodeNumber- match %d (%s) as day\n", val, str);
#endif
	*tmask = DTK_M(DAY);
	tm->tm_mday = val;

    } else if ((! (fmask & DTK_M(MONTH)))
      && ((val >= 1) && (val <= 12))) {
#ifdef DATEDEBUG
printf( "DecodeNumber- (2) match %d (%s) as month\n", val, str);
#endif
	*tmask = DTK_M(MONTH);
	tm->tm_mon = val;

    } else if (! (fmask & DTK_M(YEAR))) {
#ifdef DATEDEBUG
printf( "DecodeNumber- (2) match %d (%s) as year\n", val, str);
#endif
	*tmask = DTK_M(YEAR);
	tm->tm_year = val;
	if (tm->tm_year < 70) {
	    tm->tm_year += 2000;
	} else if (tm->tm_year < 100) {
	    tm->tm_year += 1900;
	};

    } else {
	return -1;
    };

    return 0;
} /* DecodeNumber() */


/* DecodeNumberField()
 * Interpret numeric string as a concatenated date field.
 */
int
DecodeNumberField( int len, char *str, int fmask, int *tmask, struct tm *tm, double *fsec)
{
    char *cp;

    /* yyyymmdd? */
    if (len == 8) {
#ifdef DATEDEBUG
printf( "DecodeNumberField- %s is 8 character date fmask=%08x tmask=%08x\n", str, fmask, *tmask);
#endif

	*tmask = DTK_DATE_M;

	tm->tm_mday = atoi(str+6);
	*(str+6) = '\0';
	tm->tm_mon = atoi(str+4);
	*(str+4) = '\0';
	tm->tm_year = atoi(str+0);

    /* yymmdd or hhmmss? */
    } else if (len == 6) {
#ifdef DATEDEBUG
printf( "DecodeNumberField- %s is 6 characters fmask=%08x tmask=%08x\n", str, fmask, *tmask);
#endif
	if (fmask & DTK_DATE_M) {
#ifdef DATEDEBUG
printf( "DecodeNumberField- %s is time field fmask=%08x tmask=%08x\n", str, fmask, *tmask);
#endif
	    *tmask = DTK_TIME_M;
	    tm->tm_sec = atoi(str+4);
	    *(str+4) = '\0';
	    tm->tm_min = atoi(str+2);
	    *(str+2) = '\0';
	    tm->tm_hour = atoi(str+0);

	} else {
#ifdef DATEDEBUG
printf( "DecodeNumberField- %s is date field fmask=%08x tmask=%08x\n", str, fmask, *tmask);
#endif
	    *tmask = DTK_DATE_M;
	    tm->tm_mday = atoi(str+4);
	    *(str+4) = '\0';
	    tm->tm_mon = atoi(str+2);
	    *(str+2) = '\0';
	    tm->tm_year = atoi(str+0);
	};

    } else if (strchr(str,'.') != NULL) {
#ifdef DATEDEBUG
printf( "DecodeNumberField- %s is time field fmask=%08x tmask=%08x\n", str, fmask, *tmask);
#endif
	    *tmask = DTK_TIME_M;
	    tm->tm_sec = strtod( (str+4), &cp);
	    if (cp == (str+4)) return -1;
	    if (*cp == '.') {
		*fsec = strtod( cp, NULL);
	    };
	    *(str+4) = '\0';
	    tm->tm_min = strtod( (str+2), &cp);
	    *(str+2) = '\0';
	    tm->tm_hour = strtod( (str+0), &cp);

    } else {
	return -1;
    };

    return 0;
} /* DecodeNumberField() */


/* DecodeTimezone()
 * Interpret string as a numeric timezone.
 */
int DecodeTimezone( char *str, int *tzp)
{
    int tz;
    int hr, min;
    char *cp;
    int len;

    /* assume leading character is "+" or "-" */
    hr = strtol( (str+1), &cp, 10);

    /* explicit delimiter? */
    if (*cp == ':') {
	min = strtol( (cp+1), &cp, 10);

    /* otherwise, might have run things together... */
    } else if ((*cp == '\0') && ((len = strlen(str)) > 3)) {
	min = strtol( (str+len-2), &cp, 10);
	*(str+len-2) = '\0';
	hr = strtol( (str+1), &cp, 10);

    } else {
	min = 0;
    };

    tz = (hr*60+min)*60;
    if (*str == '-') tz = -tz;

    *tzp = -tz;
    return( *cp != '\0');
} /* DecodeTimezone() */


/* DecodeSpecial()
 * Decode text string using lookup table.
 * Implement a cache lookup since it is likely that dates
 *  will be related in format.
 */
int
DecodeSpecial(int field, char *lowtoken, int *val)
{
    int type;
    datetkn *tp;

#if USE_DATE_CACHE
    if ((datecache[field] != NULL)
      && (strncmp(lowtoken,datecache[field]->token,TOKMAXLEN) == 0)) {
	tp = datecache[field];
    } else {
#endif
	tp = datebsearch(lowtoken, datetktbl, szdatetktbl);
#if USE_DATE_CACHE
    };
    datecache[field] = tp;
#endif
    if (tp == NULL) {
	type = IGNORE;
	*val = 0;
    } else {
	type = tp->type;
	if ((type == TZ) || (type == DTZ)) {
	    *val = FROMVAL(tp);
	} else {
	    *val = tp->value;
	};
    };

    return(type);
} /* DecodeSpecial() */


/* DecodeDateDelta()
 * Interpret previously parsed fields for general time interval.
 * Return 0 if decoded and -1 if problems.
 *
 * If code is changed to read fields from first to last,
 *  then use READ_FORWARD-bracketed code to allow sign
 *  to persist to subsequent unsigned fields.
 */
int
DecodeDateDelta( char *field[], int ftype[], int nf, int *dtype, struct tm *tm, double *fsec)
{
    int is_before = FALSE;
#if READ_FORWARD
    int is_neg = FALSE;
#endif

    int fmask = 0, tmask, type;
    int i, ii;
    int flen, val;
    char *cp;
    double sec;

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
    for (ii = 0; ii < nf; ii++) {
#ifdef DATEDEBUG
printf( "DecodeDateDelta- field[%d] is %s (type %d)\n", ii, field[ii], ftype[ii]);
#endif
	if (ftype[ii] == DTK_TIME) {
	    if (DecodeTime(field[ii], fmask, &tmask, tm, fsec) != 0) return -1;

	} else {
	    break;
	};
    };

    /* read through remaining list backwards to pick up units before values */
    for (i = nf-1; i >= ii; i--) {
#ifdef DATEDEBUG
printf( "DecodeDateDelta- field[%d] is %s (type %d)\n", i, field[i], ftype[i]);
#endif
	switch (ftype[i]) {
	case DTK_TIME:
#if FALSE
	    if (DecodeTime(field[i], fmask, &tmask, tm, fsec) != 0) return -1;
#endif
	    return -1;
	    break;

	case DTK_TZ:	/* timezone is a token with a leading sign character */
#if READ_FORWARD
	    is_neg = (*field[i] == '-');
#endif

	case DTK_NUMBER:
	    val = strtol( field[i], &cp, 10);
#if READ_FORWARD
	    if (is_neg && (val > 0)) val = -val;
#endif
	    if (*cp == '.') {
		*fsec = strtod( cp, NULL);
		if (val < 0) *fsec = - (*fsec);
	    };
	    flen = strlen(field[i]);
	    tmask = DTK_M(type);

	    switch (type) {
	    case DTK_MICROSEC:
		*fsec += (val * 1e-6);
		break;

	    case DTK_MILLISEC:
		*fsec += (val * 1e-3);
		break;

	    case DTK_SECOND:
		tm->tm_sec += val;
		break;

	    case DTK_MINUTE:
		tm->tm_min += val;
		break;

	    case DTK_HOUR:
		tm->tm_hour += val;
		break;

	    case DTK_DAY:
		tm->tm_mday += val;
		break;

	    case DTK_WEEK:
		tm->tm_mday += val*7;
		break;

	    case DTK_MONTH:
		tm->tm_mon += val;
		break;

	    case DTK_YEAR:
		tm->tm_year += val;
		break;

	    case DTK_DECADE:
		tm->tm_year += val*10;
		break;

	    case DTK_CENTURY:
		tm->tm_year += val*100;
		break;

	    case DTK_MILLENIUM:
		tm->tm_year += val*1000;
		break;

	    default:
		return -1;
	    };
	    break;

	case DTK_STRING:
	case DTK_SPECIAL:
	    type = DecodeUnits( i, field[i], &val);
#ifdef DATEDEBUG
printf( "DecodeDateDelta- special field[%d] %s type=%d value=%d\n", i, field[i], type, val); 
#endif
	    if (type == IGNORE) continue;

	    tmask = 0; /* DTK_M(type); */
	    switch (type) {
	    case UNITS:
#ifdef DATEDEBUG
printf( "DecodeDateDelta- UNITS field %s value is %d\n", field[i], val); 
#endif
		type = val;
		break;

	    case AGO:
		is_before = TRUE;
		type = val;
		break;

	    case RESERV:
		type = (DTK_DATE_M || DTK_TIME_M);
		*dtype = val;
		break;

	    default:
		return -1;
	    };
	    break;

	default:
	    return -1;
	};

#ifdef DATEDEBUG
printf( "DecodeDateDelta- (%08x/%08x) field[%d] %s value is %d\n",
 fmask, tmask, i, field[i], val); 
#endif

	if (tmask & fmask) return -1;
	fmask |= tmask;
    };

    if (*fsec != 0) {
	*fsec = modf( *fsec, &sec);
	tm->tm_sec += sec;
    };

    if (is_before) {
	*fsec = -(*fsec);
	tm->tm_sec = -(tm->tm_sec);
	tm->tm_min = -(tm->tm_min);
	tm->tm_hour = -(tm->tm_hour);
	tm->tm_mday = -(tm->tm_mday);
	tm->tm_mon = -(tm->tm_mon);
	tm->tm_year = -(tm->tm_year);
    };

#ifdef DATEDEBUG
printf( "DecodeDateDelta- mask %08x (%08x)", fmask, DTK_DATE_M);
printf( " set y%04d m%02d d%02d", tm->tm_year, tm->tm_mon, tm->tm_mday);
printf( " %02d:%02d:%02d\n", tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif

    /* ensure that at least one time field has been found */
    return((fmask != 0)? 0: -1);
} /* DecodeDateDelta() */


/* DecodeUnits()
 * Decode text string using lookup table.
 * This routine supports time interval decoding.
 */
int
DecodeUnits(int field, char *lowtoken, int *val)
{
    int type;
    datetkn *tp;

#if USE_DATE_CACHE
    if ((deltacache[field] != NULL)
      && (strncmp(lowtoken,deltacache[field]->token,TOKMAXLEN) == 0)) {
	tp = deltacache[field];
    } else {
#endif
	tp = datebsearch(lowtoken, deltatktbl, szdeltatktbl);
#if USE_DATE_CACHE
    };
    deltacache[field] = tp;
#endif
    if (tp == NULL) {
	type = IGNORE;
	*val = 0;
    } else {
	type = tp->type;
	if ((type == TZ) || (type == DTZ)) {
	    *val = FROMVAL(tp);
	} else {
	    *val = tp->value;
	};
    };

    return(type);
} /* DecodeUnits() */


/*
 * Binary search -- from Knuth (6.2.1) Algorithm B.  Special case like this
 * is WAY faster than the generic bsearch().
 */
datetkn *
datebsearch(char *key, datetkn *base, unsigned int nel)
{
    register datetkn *last = base + nel - 1, *position;
    register int result;

    while (last >= base) {
	position = base + ((last - base) >> 1);
	result = key[0] - position->token[0];
	if (result == 0) {
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


/***************************************************************************/
/***************************************************************************/
/***************************************************************************/

char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL};

char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
 "Thursday", "Friday", "Saturday", NULL};

#if FALSE
int EncodeMonth(int mon, char *str);

int EncodeMonth(int mon, char *str)
{
    strcpy( str, months[mon-1]);

    return(TRUE);
} /* EncodeMonth() */
#endif

#define EncodeMonth(m,s)	strcpy(s,months[m-1])


int EncodeSpecialDateTime(DateTime dt, char *str)
{
    if (DATETIME_IS_RESERVED(dt)) {
	if (DATETIME_IS_INVALID(dt)) {
	    strcpy( str, INVALID);

	} else if (DATETIME_IS_NOBEGIN(dt)) {
	    strcpy( str, EARLY);

	} else if (DATETIME_IS_NOEND(dt)) {
	    strcpy( str, LATE);

	} else if (DATETIME_IS_CURRENT(dt)) {
	    strcpy( str, DCURRENT);

	} else if (DATETIME_IS_EPOCH(dt)) {
	    strcpy( str, EPOCH);

	} else {
#ifdef DATEDEBUG
printf( "EncodeSpecialDateTime- unrecognized date\n");
#endif
	    strcpy( str, INVALID);
	};
	return(TRUE);
    };

    return(FALSE);
} /* EncodeSpecialDateTime() */


int EncodePostgresDate(struct tm *tm, double fsec, char *str)
{
    char mabbrev[4], dabbrev[4];
    int day;
    double sec;

    sec = (tm->tm_sec + fsec);

    tm->tm_isdst = -1;

#ifdef DATEDEBUG
printf( "EncodePostgresDate- timezone is %s; offset is %d; daylight is %d\n",
 CTZName, CTimeZone, CDayLight);
#endif

    day = date2j( tm->tm_year, tm->tm_mon, tm->tm_mday);
#ifdef DATEDEBUG
printf( "EncodePostgresDate- day is %d\n", day);
#endif
    tm->tm_wday = j2day( day);

    strncpy( dabbrev, days[tm->tm_wday], 3);
    dabbrev[3] = '\0';

    if ((tm->tm_mon < 1) || (tm->tm_mon > 12))
	return -1;

    strcpy( mabbrev, months[tm->tm_mon-1]);

    if (DateStyle == USE_ISO_DATES) {
	if (tm->tm_year > 0) {
	    sprintf( str, "%04d-%02d-%02d %02d:%02d:%5.2f %s",
	      tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, sec, CTZName);
	    /* XXX brute-force fill in leading zero on seconds */
	    if (*(str+17) == ' ') *(str+17) = '0';

	} else {
	    sprintf( str, "%04d-%02d-%02d %02d:%02d %s",
	      -(tm->tm_year-1), tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, "BC");
	};

    } else if (DateStyle == USE_SQL_DATES) {
	if (EuroDates) {
	    sprintf( str, "%02d/%02d", tm->tm_mday, tm->tm_mon);
	} else {
	    sprintf( str, "%02d/%02d", tm->tm_mon, tm->tm_mday);
	};
	if (tm->tm_year > 0) {
	    sprintf( (str+5), "/%04d %02d:%02d:%5.2f %s",
	      tm->tm_year, tm->tm_hour, tm->tm_min, sec, CTZName);
	    /* XXX brute-force fill in leading zero on seconds */
	    if (*(str+17) == ' ') *(str+17) = '0';

	} else {
	    sprintf( (str+5), "/%04d %02d:%02d %s",
	      -(tm->tm_year-1), tm->tm_hour, tm->tm_min, "BC");
	};

    } else { /* if (DateStyle == USE_POSTGRES_DATES) */
	sprintf( str, "%3s ", dabbrev);
	if (EuroDates) {
	    sprintf( (str+4), "%02d %3s", tm->tm_mday, mabbrev);
	} else {
	    sprintf( (str+4), "%3s %02d", mabbrev, tm->tm_mday);
	};
	if (tm->tm_year > 0) {
	    sprintf( (str+10), " %02d:%02d:%5.2f %04d %s",
	      tm->tm_hour, tm->tm_min, sec, tm->tm_year, CTZName);
	    /* XXX brute-force fill in leading zero on seconds */
	    if (*(str+17) == ' ') *(str+17) = '0';

	} else {
	    sprintf( (str+10), " %02d:%02d %04d %s",
	      tm->tm_hour, tm->tm_min, -(tm->tm_year-1), "BC");
	};
    };

#ifdef DATEDEBUG
printf( "EncodePostgresDate- date result is %s\n", str);
#endif

#ifdef DATEDEBUG
    if (tm->tm_year >= 1000) tm->tm_year -= 1900;
    tm->tm_mon -= 1;
    strftime( buf, sizeof(buf), "%y.%m.%d %H:%M:%S %Z", tm);
printf( "EncodePostgresDate- strftime result is %s\n", buf);
#endif

    return(TRUE);
} /* EncodePostgresDate() */


int EncodePostgresSpan(struct tm *tm, double fsec, char *str)
{
    int is_before = FALSE;
    int is_nonzero = FALSE;
    char *cp;

    strcpy( str, "@");
    cp = str+strlen(str);
    if (tm->tm_year != 0) {
	is_nonzero = TRUE;
	is_before |= (tm->tm_year < 0);
	sprintf( cp, " %d year%s", abs(tm->tm_year), ((abs(tm->tm_year) != 1)? "s": ""));
	cp += strlen(cp);
    };
    if (tm->tm_mon != 0) {
	is_nonzero = TRUE;
	is_before |= (tm->tm_mon < 0);
	sprintf( cp, " %d mon%s", abs(tm->tm_mon), ((abs(tm->tm_mon) != 1)? "s": ""));
	cp += strlen(cp);
    };
    if (tm->tm_mday != 0) {
	is_nonzero = TRUE;
	is_before |= (tm->tm_mday < 0);
	sprintf( cp, " %d day%s", abs(tm->tm_mday), ((abs(tm->tm_mday) != 1)? "s": ""));
	cp += strlen(cp);
    };
    if (tm->tm_hour != 0) {
	is_nonzero = TRUE;
	is_before |= (tm->tm_hour < 0);
	sprintf( cp, " %d hour%s", abs(tm->tm_hour), ((abs(tm->tm_hour) != 1)? "s": ""));
	cp += strlen(cp);
    };
    if (tm->tm_min != 0) {
	is_nonzero = TRUE;
	is_before |= (tm->tm_min < 0);
	sprintf( cp, " %d min%s", abs(tm->tm_min), ((abs(tm->tm_min) != 1)? "s": ""));
	cp += strlen(cp);
    };
    if (tm->tm_sec != 0) {
	is_nonzero = TRUE;
	is_before |= (tm->tm_sec < 0);
	sprintf( cp, " %d sec%s", abs(tm->tm_sec), ((abs(tm->tm_sec) != 1)? "s": ""));
	cp += strlen(cp);
    };

    if (! is_nonzero) {
	strcat( cp, " 0");
	cp += strlen(cp);
    };

    if (is_before) {
	strcat( cp, " ago");
	cp += strlen(cp);
    };

#ifdef DATEDEBUG
printf( "EncodePostgresSpan- result is %s\n", str);
#endif

    return 0;
} /* EncodePostgresSpan() */
