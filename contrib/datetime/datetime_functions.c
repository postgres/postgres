/*
 * datetime_functions.c --
 *
 * This file defines new functions for the time and date data types.
 *
 * Copyright (c) 1996, Massimo Dal Zotto <dz@cs.unitn.it>
 */

#include <time.h>

#include "postgres.h"
#include "utils/palloc.h"
#include "utils/datetime.h"


TimeADT    *
time_difference(TimeADT *time1, TimeADT *time2)
{
	TimeADT    *result = (TimeADT *) palloc(sizeof(TimeADT));

	*result = *time1 - *time2;
	return (result);
}

TimeADT    *
currenttime()
{
	time_t		current_time;
	struct tm  *tm;
	TimeADT    *result = (TimeADT *) palloc(sizeof(TimeADT));

	current_time = time(NULL);
	tm = localtime(&current_time);
	*result = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec);
	return (result);
}

DateADT
currentdate()
{
	time_t		current_time;
	struct tm  *tm;
	DateADT		result;

	current_time = time(NULL);
	tm = localtime(&current_time);

	result = date2j(tm->tm_year, tm->tm_mon + 1, tm->tm_mday) -
		date2j(100, 1, 1);
	return (result);
}

int4
hours(TimeADT *time)
{
	return (*time / (60 * 60));
}

int4
minutes(TimeADT *time)
{
	return (((int) (*time / 60)) % 60);
}

int4
seconds(TimeADT *time)
{
	return (((int) *time) % 60);
}

int4
day(DateADT *date)
{
	struct tm	tm;

	j2date((*date + date2j(2000, 1, 1)),
		   &tm.tm_year, &tm.tm_mon, &tm.tm_mday);

	return (tm.tm_mday);
}

int4
month(DateADT *date)
{
	struct tm	tm;

	j2date((*date + date2j(2000, 1, 1)),
		   &tm.tm_year, &tm.tm_mon, &tm.tm_mday);

	return (tm.tm_mon);
}

int4
year(DateADT *date)
{
	struct tm	tm;

	j2date((*date + date2j(2000, 1, 1)),
		   &tm.tm_year, &tm.tm_mon, &tm.tm_mday);

	return (tm.tm_year);
}

int4
asminutes(TimeADT *time)
{
	int			seconds = (int) *time;

	return (seconds / 60);
}

int4
asseconds(TimeADT *time)
{
	int			seconds = (int) *time;

	return (seconds);
}
