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

	abstime2tm(timestamp, &tz, tm, tzn);
	EncodeDateTime(tm, fsec, &tz, &tzn, USE_ISO_DATES, buf);

	result = palloc(strlen(buf) + 1);
	strcpy(result, buf);
	return result;
}

time_t
now(void)
{
	time_t		sec;

	sec = GetCurrentTransactionStartTime();
	return (sec);
}

bool
timestampeq(time_t t1, time_t t2)
{
#if FALSE
	return(t1 == t2);
#endif
	return(abstimeeq(t1,t2));
}

bool
timestampne(time_t t1, time_t t2)
{
#if FALSE
	return(t1 != t2);
#endif
	return(abstimene(t1,t2));
}

bool
timestamplt(time_t t1, time_t t2)
{
#if FALSE
	return(t1 > t2);
#endif
	return(abstimelt(t1,t2));
}

bool
timestampgt(time_t t1, time_t t2)
{
#if FALSE
	return(t1 < t2);
#endif
	return(abstimegt(t1,t2));
}

bool
timestample(time_t t1, time_t t2)
{
#if FALSE
	return(t1 >= t2);
#endif
	return(abstimele(t1,t2));
}

bool
timestampge(time_t t1, time_t t2)
{
#if FALSE
	return(t1 <= t2);
#endif
	return(abstimege(t1,t2));
}

DateTime   *
timestamp_datetime(time_t timestamp)
{
	DateTime   *result;

	double		fsec = 0;
	struct tm  *tm;

	if (!PointerIsValid(result = PALLOCTYPE(DateTime)))
		elog(WARN, "Memory allocation failed, can't convert timestamp to datetime", NULL);

	tm = localtime((time_t *) &timestamp);
	tm->tm_year += 1900;
	tm->tm_mon += 1;

	if (tm2datetime(tm, fsec, NULL, result) != 0)
		elog(WARN, "Unable to convert timestamp to datetime", timestamp_out(timestamp));

	return (result);
} /* timestamp_datetime() */
