#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "postgres.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "access/xact.h"

time_t
timestamp_in(const char *timestamp_str)
{
	int4		result;

	result = nabstimein((char *) timestamp_str);

	return result;
}

char *
timestamp_out(time_t timestamp)
{
	char	   *result;
	int			tz;
	double		fsec = 0;
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];
	char		zone[MAXDATELEN + 1],
			   *tzn = zone;

	switch (timestamp)
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
			abstime2tm(timestamp, &tz, tm, tzn);
			EncodeDateTime(tm, fsec, &tz, &tzn, USE_ISO_DATES, buf);
			break;
	}

	result = palloc(strlen(buf) + 1);
	strcpy(result, buf);
	return result;
}	/* timestamp_out() */

time_t
now(void)
{
	time_t		sec;

	sec = GetCurrentTransactionStartTime();
	return sec;
}

bool
timestampeq(time_t t1, time_t t2)
{
	return abstimeeq(t1, t2);
}

bool
timestampne(time_t t1, time_t t2)
{
	return abstimene(t1, t2);
}

bool
timestamplt(time_t t1, time_t t2)
{
	return abstimelt(t1, t2);
}

bool
timestampgt(time_t t1, time_t t2)
{
	return abstimegt(t1, t2);
}

bool
timestample(time_t t1, time_t t2)
{
	return abstimele(t1, t2);
}

bool
timestampge(time_t t1, time_t t2)
{
	return abstimege(t1, t2);
}

DateTime   *
timestamp_datetime(time_t timestamp)
{
	return abstime_datetime((AbsoluteTime) timestamp);
}	/* timestamp_datetime() */

time_t
datetime_timestamp(DateTime *datetime)
{
	return (AbsoluteTime) datetime_abstime(datetime);
}	/* datetime_timestamp() */
