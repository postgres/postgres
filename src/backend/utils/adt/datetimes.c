/*-------------------------------------------------------------------------
 *
 * datetimes.c--
 *    implements DATE and TIME data types specified in SQL-92 standard
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/datetimes.c,v 1.3 1996/08/27 07:32:30 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "config.h"

#include <stdio.h>		/* for sprintf() */
#include <string.h>
#include "postgres.h"
#include "utils/palloc.h"
#include "utils/elog.h"

/* these things look like structs, but we pass them by value so be careful
   For example, passing an int -> DateADT is not portable! */
typedef struct DateADT {
    char	day;
    char	month;
    short	year;
} DateADT;

typedef struct TimeADT {
    short	hr;
    short	min;
    float	sec;
} TimeADT;

#ifndef EUROPEAN_STYLE
#define AMERICAN_STYLE
#endif

static int	day_tab[2][12] = {
	{31,28,31,30,31,30,31,31,30,31,30,31},
	{31,29,31,30,31,30,31,31,30,31,30,31}  };

static int
isleap(int year)
{
    return
	(((year % 4) == 0 && (year % 100) != 0) || (year % 400) == 0);
}

/*****************************************************************************
 *   Date ADT
 *****************************************************************************/

int4
date_in(char *datestr)
{
    int d, m, y;
    int4 result;
    DateADT *date = (DateADT*)&result;

#if 0
# ifdef USE_SHORT_YEAR
#  define CHECK_DATE_LEN(datestr) (strlen(datestr) >= 8)
# else
#  define CHECK_DATE_LEN(datestr) (strlen(datestr) == 10)
# endif /* USE_SHORT_YEAR */
#else
# define CHECK_DATE_LEN(datestr) 1
#endif

#ifdef AMERICAN_STYLE
    if (!CHECK_DATE_LEN(datestr) ||
	sscanf(datestr, "%d%*c%d%*c%d", &m, &d, &y) != 3) {
	elog(WARN, "date_in: date \"%s\" not of the form mm-dd-yyyy",
	     datestr);
    }
#else
    if (!CHECK_DATE_LEN(datestr) ||
	sscanf(datestr, "%d%*c%d%*c%d", &d, &m, &y) != 3) {
	elog(WARN, "date_in: date \"%s\" not of the form dd-mm-yyyy",
	     datestr);
    }
#endif
    if (y < 0 || y > 32767)
	elog(WARN, "date_in: year must be limited to values 0 through 32767 in \"%s\"", datestr);
    if (m < 1 || m > 12)
	elog(WARN, "date_in: month must be limited to values 1 through 12 in \"%s\"", datestr);
    if (d < 1 || d > day_tab[isleap(y)][m-1])
	elog(WARN, "date_in: day must be limited to values 1 through %d in \"%s\"",
	     day_tab[isleap(y)][m-1], datestr);

#ifdef USE_SHORT_YEAR
    if (y < 100) 
	y += 1900; /* hack! */
#endif /* USE_SHORT_YEAR */
 
    date->day = d;
    date->month = m;
    date->year = y;
    return result;
}

char *
date_out(int4 dateVal)
{
    char *datestr = palloc(11);
    int4 dateStore;
    DateADT *date;

       /* DateADT is a structure that happens to be four bytes long,
          trust me on this.... */
    date = (DateADT*)&dateStore;
    dateStore = dateVal;

#ifdef AMERICAN_STYLE
    sprintf(datestr, "%02d-%02d-%04d",
            (int)date->month, (int)date->day, (int)date->year);
#else
    sprintf(datestr, "%02d-%02d-%04d",
            (int)date->day, (int)date->month, (int)date->year);
#endif

    return datestr;
}


int
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

int
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

int
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

int
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

int
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

int
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

/*****************************************************************************
 *   Time ADT
 *****************************************************************************/

char *
time_in(char *timestr)
{
    int h, m;
    float sec;
    TimeADT *time;

    if (sscanf(timestr, "%d%*c%d%*c%f", &h, &m, &sec) != 3) {
	sec = 0.0;
	if (sscanf(timestr, "%d%*c%d", &h, &m) != 2) {
	    elog(WARN, "time_in: time \"%s\" not of the form hh:mm:ss",
		 timestr);
	}
    }

    if (h < 0 || h > 23)
	elog(WARN, "time_in: hour must be limited to values 0 through 23 in \"%s\"", timestr);
    if (m < 0 || m > 59)
	elog(WARN, "time_in: minute must be limited to values 0 through 59 in \"%s\"", timestr);
    if (sec < 0 || sec >= 60.0)
	elog(WARN, "time_in: second must be limited to values 0 through 59.999 in \"%s\"", timestr);

    time = (TimeADT*)palloc(sizeof(TimeADT));
    time->hr = h;
    time->min = m;
    time->sec = sec;
    return (char*)time;
}

char *
time_out(TimeADT *time)
{
    char *timestr = palloc(32);
    int n;
    float f;

    if (time->sec == 0.0) {
	sprintf(timestr, "%02d:%02d",
		(int)time->hr, (int)time->min);
    } else {
	n = (int)time->sec;
	f = (float)n;
	if (f == time->sec) {
	    sprintf(timestr, "%02d:%02d:%02d",
		    (int)time->hr, (int)time->min, n);
	} else {
	    sprintf(timestr, "%02d:%02d:%09.6f",
		    (int)time->hr, (int)time->min, time->sec);
	}
    }

    return timestr;
}


int
time_eq(TimeADT *time1, TimeADT *time2)
{
    return (time1->sec==time2->sec && time1->min==time2->min &&
	    time1->hr==time2->hr);
}

int
time_ne(TimeADT *time1, TimeADT *time2)
{
    return (time1->sec!=time2->sec || time1->min!=time2->min ||
	    time1->hr!=time2->hr);
}

int
time_lt(TimeADT *time1, TimeADT *time2)
{
    if (time1->hr!=time2->hr)
	return (time1->hr<time2->hr);
    if (time1->min!=time2->min)
	return (time1->min<time2->min);
    return (time1->sec<time2->sec);
}

int
time_le(TimeADT *time1, TimeADT *time2)
{
    if (time1->hr!=time2->hr)
	return (time1->hr<=time2->hr);
    if (time1->min!=time2->min)
	return (time1->min<=time2->min);
    return (time1->sec<=time2->sec);
}

int
time_gt(TimeADT *time1, TimeADT *time2)
{
    if (time1->hr!=time2->hr)
	return (time1->hr>time2->hr);
    if (time1->min!=time2->min)
	return (time1->min>time2->min);
    return (time1->sec>time2->sec);
}

int
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

int32   /* RelativeTime */
int42reltime(int32 timevalue)
{
    return(timevalue);
}
