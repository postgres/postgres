/*
 * datetime_functions.c --
 *
 * This file defines new functions for the time and date data types.
 *
 * Copyright (c) 1996, Massimo Dal Zotto <dz@cs.unitn.it>
 */

#include <time.h>

#include "postgres.h"
#include "pg_type.h"
#include "utils/palloc.h"

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

TimeADT *
time_difference(TimeADT *time1, TimeADT *time2)
{
    TimeADT *time = (TimeADT*)palloc(sizeof(TimeADT));

    time->sec = time1->sec - time2->sec;
    time->min = time1->min - time2->min;
    time->hr  = time1->hr  - time2->hr;

    if (time->sec < 0) {
	time->sec += 60.0;
	time->min--;
    } else if (time->sec >= 60.0) {
	time->sec -= 60.0;
	time->min++;
    }

    if (time->min < 0) {
	time->min += 60;
	time->hr--;
    } else if (time->min >= 60) {
	time->min -= 60;
	time->hr++;
    }

    if (time->hr < 0) {
	time->hr += 24;
    } else if (time->hr >= 24) {
	time->hr -= 24;
    }

    return (time);
}

TimeADT *
currentTime()
{
    time_t current_time;
    struct tm *tm;
    TimeADT *result = (TimeADT*)palloc(sizeof(TimeADT));

    current_time = time(NULL);
    tm = localtime(&current_time);
    result->sec = tm->tm_sec;
    result->min = tm->tm_min;
    result->hr  = tm->tm_hour;

    return (result);
}

int4
currentDate()
{
    time_t current_time;
    struct tm *tm;
    int4 result;
    DateADT *date = (DateADT*)&result;

    current_time = time(NULL);
    tm = localtime(&current_time);
    date->day   = tm->tm_mday;
    date->month = tm->tm_mon+1;
    date->year  = tm->tm_year+1900;

    return (result);
}

int4
hours(TimeADT *time)
{
    return (time->hr);
}

int4
minutes(TimeADT *time)
{
    return (time->min);
}

int4
seconds(TimeADT *time)
{
    int seconds = (int)time->sec;
    return (seconds);
}

int4
day(int4 val)
{
    DateADT *date = (DateADT*)&val;
    return (date->day);
}

int4
month(int4 val)
{
    DateADT *date = (DateADT*)&val;
    return (date->month);
}

int4
year(int4 val)
{
    DateADT *date = (DateADT*)&val;
    return (date->year);
}

int4
asMinutes(TimeADT *time)
{
    int seconds = (int)time->sec;
    return (time->min + 60*time->hr);
}

int4
asSeconds(TimeADT *time)
{
    int seconds = (int)time->sec;
    return (seconds + 60*time->min + 3600*time->hr);
}

