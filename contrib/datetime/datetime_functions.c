/*
 * datetime_functions.c --
 *
 * This file defines new functions for the time and date data types.
 *
 * Copyright (C) 1999, Massimo Dal Zotto <dz@cs.unitn.it>
 *
 * Date2mjd code contributed by Reiner Dassing <dassing@wettzell.ifag.de>
 *
 * This software is distributed under the GNU General Public License
 * either version 2, or (at your option) any later version.
 */

#include <stdio.h>
#include <string.h>
#include <limits.h>
#ifdef HAVE_FLOAT_H
#include <float.h>
#endif

#include "postgres.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/nabstime.h"
#include "utils/datetime.h"
#include "access/xact.h"

#include "datetime_functions.h"

/* Constant to replace calls to date2j(2000,1,1) */
#define JDATE_2000	2451545

/*
 * decode_24h_time()
 *
 * Decode time string 00:00:00 through 24:00:00.
 */
static int
decode_24h_time(char *str, struct tm * tm, double *fsec)
{
	char	   *cp;

	tm->tm_hour = strtol(str, &cp, 10);
	if (*cp != ':')
		return -1;
	str = cp + 1;
	tm->tm_min = strtol(str, &cp, 10);
	if (*cp == '\0')
	{
		tm->tm_sec = 0;
		*fsec = 0;
	}
	else if (*cp != ':')
		return -1;
	else
	{
		str = cp + 1;
		tm->tm_sec = strtol(str, &cp, 10);
		if (*cp == '\0')
			*fsec = 0;
		else if (*cp == '.')
		{
			str = cp;
			*fsec = strtod(str, &cp);
			if (cp == str)
				return -1;
		}
		else
			return -1;
	}

	/* do a sanity check */
	if ((tm->tm_hour < 0) || (tm->tm_hour > 24)
		|| (tm->tm_min < 0) || (tm->tm_min > 59)
		|| (tm->tm_sec < 0) || (tm->tm_sec > 59)
		|| (*fsec < 0))
		return -1;

	return 0;
}

/*
 * A modified version of time_in which allows the value 24:00:00 for
 * time and converts it to TimeADT data type forcing seconds to 0.
 * This can be useful if you need to handle TimeADT values limited
 * to hh:mm like in timetables.
 */

TimeADT    *
hhmm_in(char *str)
{
	TimeADT    *time;

	double		fsec;
	struct tm	tt,
			   *tm = &tt;

	if (!PointerIsValid(str))
		elog(ERROR, "Bad (null) time external representation", NULL);

	if (decode_24h_time(str, tm, &fsec) != 0)
		elog(ERROR, "Bad time external representation '%s'", str);

	if ((tm->tm_hour < 0) || (tm->tm_hour > 24)
		|| ((tm->tm_hour == 24)
			&& ((tm->tm_min != 0) || (tm->tm_sec != 0) || (fsec != 0.0))))
	{
		elog(ERROR,
			 "Time must be limited to values 00:00:00 through 24:00:00 "
			 "in \"%s\"",
			 str);
	}

	time = palloc(sizeof(TimeADT));
	*time = ((((tm->tm_hour * 60) + tm->tm_min) * 60));

	return (time);
}

/*
 * A modified version of time_out which converts from TimeADT data type
 * omitting the seconds field when it is 0.
 * Useful if you need to handle TimeADT values limited to hh:mm.
 */

char *
hhmm_out(TimeADT *time)
{
	char	   *result;
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];

	if (!PointerIsValid(time))
		return NULL;

	tm->tm_hour = (*time / (60 * 60));
	tm->tm_min = (((int) (*time / 60)) % 60);
	tm->tm_sec = (((int) *time) % 60);

	if (tm->tm_sec == 0)
		sprintf(buf, "%02d:%02d", tm->tm_hour, tm->tm_min);
	else
		sprintf(buf, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);

	result = palloc(strlen(buf) + 1);
	strcpy(result, buf);

	return (result);
}

TimeADT    *
hhmm(TimeADT *time)
{
	TimeADT    *result = palloc(sizeof(TimeADT));

	*result = (((int) *time) / 60 * 60);

	return (result);
}

TimeADT    *
time_difference(TimeADT *time1, TimeADT *time2)
{
	TimeADT    *time = palloc(sizeof(TimeADT));

	*time = (*time1 - *time2);
	return (time);
}

int4
time_hours(TimeADT *time)
{
	return (((int) *time) / 3600);
}

int4
time_minutes(TimeADT *time)
{
	return ((((int) *time) / 60) % 60);
}

int4
time_seconds(TimeADT *time)
{
	return (((int) *time) % 60);
}

int4
as_minutes(TimeADT *time)
{
	return (((int) *time) / 60);
}

int4
as_seconds(TimeADT *time)
{
	return ((int) *time);
}

int4
date_day(DateADT val)
{
	int			year,
				month,
				day;

	j2date(val + JDATE_2000, &year, &month, &day);

	return (day);
}

int4
date_month(DateADT val)
{
	int			year,
				month,
				day;

	j2date(val + JDATE_2000, &year, &month, &day);

	return (month);
}

int4
date_year(DateADT val)
{
	int			year,
				month,
				day;

	j2date(val + JDATE_2000, &year, &month, &day);

	return (year);
}

TimeADT    *
currenttime()
{
	TimeADT    *result = palloc(sizeof(TimeADT));
	struct tm  *tm;
	time_t		current_time;

	current_time = time(NULL);
	tm = localtime(&current_time);
	*result = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec);

	return (result);
}

DateADT
currentdate()
{
	DateADT		date;
	struct tm	tt,
			   *tm = &tt;

	GetCurrentTime(tm);
	date = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - JDATE_2000);
	return (date);
}

int4
date2mjd(DateADT val)
{
	int			result;

	result = val + JDATE_2000 - 2400000.5;

	return result;
}

/* end of file */

/*
 * Local Variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */
