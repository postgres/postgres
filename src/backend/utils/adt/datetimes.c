/*-------------------------------------------------------------------------
 *
 * datetimes.c--
 *    implements DATE and TIME data types specified in SQL-92 standard
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/datetimes.c,v 1.9 1997/03/02 02:05:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>		/* for sprintf() */
#include <string.h>

#include <postgres.h>
#include <miscadmin.h>
#include <utils/builtins.h>
#include <utils/datetime.h>

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

    if (EuroDates == 1) { /* Expect european format dates */
        if (!CHECK_DATE_LEN(datestr) ||
          sscanf(datestr, "%d%*c%d%*c%d", &d, &m, &y) != 3) {
            elog(WARN, "date_in: date \"%s\" not of the form dd-mm-yyyy",
             datestr);
        }
    } else {
        if (!CHECK_DATE_LEN(datestr) ||
          sscanf(datestr, "%d%*c%d%*c%d", &m, &d, &y) != 3) {
            elog(WARN, "date_in: date \"%s\" not of the form mm-dd-yyyy",
              datestr);
        }
    }
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

    if (EuroDates == 1) /* Output european format dates */
        sprintf(datestr, "%02d-%02d-%04d",
                (int)date->day, (int)date->month, (int)date->year);
    else
        sprintf(datestr, "%02d-%02d-%04d",
                (int)date->month, (int)date->day, (int)date->year);

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
  int4 dv1, dv2;
  DateADT *date1, *date2;
  int32 days = 0;
  int i;

  /* This circumlocution allows us to assume that date1 is always
     before date2.  */
  dv1 = date_smaller (dateVal1, dateVal2);
  dv2 = date_larger (dateVal1, dateVal2);
  date1 = (DateADT *) &dv1;
  date2 = (DateADT *) &dv2;

  /* Sum number of days in each full year between date1 and date2.  */
  for (i = date1->year + 1; i < date2->year; ++i)
    days += isleap (i) ? 366 : 365;

  if (days)
    {
      /* We need to wrap around the year.  Add in number of days in each
	 full month from date1 to end of year.  */
      for (i = date1->month + 1; i <= 12; ++i)
	days += day_tab[isleap (date1->year)][i - 1];

      /* Add in number of days in each full month from start of year to
	 date2.  */
      for (i = 1; i < date2->month; ++i)
	days += day_tab[isleap (date2->year)][i - 1];
    }
  else
    {
      /* Add in number of days in each full month from date1 to date2.  */
      for (i = date1->month + 1; i < date2->month; ++i)
	days += day_tab[isleap (date1->year)][i - 1];
    }

  if (days || date1->month != date2->month)
    {
      /* Add in number of days left in month for date1.  */
      days += day_tab[isleap (date1->year)][date1->month - 1] - date1->day;

      /* Add in day of month of date2.  */
      days += date2->day;
    }
  else
    {
      /* Everything's in the same month, so just subtract the days!  */
      days = date2->day - date1->day;
    }

  return (days);
}

/* Add a number of days to a date, giving a new date.
   Must handle both positive and negative numbers of days.  */
int4
date_pli(int4 dateVal, int32 days)
{
  DateADT *date1 = (DateADT *) &dateVal;
  /* Use separate day variable because date1->day is a narrow type.  */
  int32 day = date1->day + days;

  if (days > 0)
    {
      /* Loop as long as day has wrapped around end of month.  */
      while (day > day_tab[isleap (date1->year)][date1->month - 1])
	{
	  day -= day_tab[isleap (date1->year)][date1->month - 1];
	  if (++date1->month > 12)
	    {
	      /* Month wrapped around.  */
	      date1->month = 1;
	      ++date1->year;
	    }
	}
    }
  else
    {
      /* Loop as long as day has wrapped around beginning of month.  */
      while (day < 1)
	{
	  /* Decrement month first, because a negative day number
	     should be held as relative to the previous month's end.  */
	  if (--date1->month < 1)
	    {
	      /* Month wrapped around.  */
	      date1->month = 12;
	      --date1->year;
	    }

	  day += day_tab[isleap (date1->year)][date1->month - 1];
	}
    }
  date1->day = day;

  return (dateVal);
}

/* Subtract a number of days from a date, giving a new date.  */
int4
date_mii(int4 dateVal, int32 days)
{
  return (date_pli (dateVal, -days));
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
