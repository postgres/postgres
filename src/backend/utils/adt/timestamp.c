#include <stdio.h>
#include <string.h>
#include <time.h>
#include "postgres.h"
#include "utils/builtins.h"

time_t
timestamp_in(const char *timestamp_str)
{
    struct tm input_time;
    int4 result;

    memset(&input_time, 0, sizeof(input_time));
    if(sscanf(timestamp_str, "%d%*c%d%*c%d%*c%d%*c%d%*c%d",
	      &input_time.tm_year, &input_time.tm_mon, &input_time.tm_mday,
	      &input_time.tm_hour, &input_time.tm_min, &input_time.tm_sec) != 6) {
	elog(WARN, "timestamp_in: timestamp \"%s\" not of the form yyyy-mm-dd hh:mm:ss",
	     timestamp_str);
    }

    /* range checking?  bahahahaha.... */

    input_time.tm_year -= 1900;
    input_time.tm_mon -= 1;

    /* use mktime(), but make this GMT, not local time */
    result = mktime(&input_time);

    return result;
}

char *
timestamp_out(time_t timestamp)
{
    char *result;
    struct tm *time;

    time = localtime((time_t *)&timestamp);
    result = palloc(20);
    sprintf(result, "%04d-%02d-%02d %02d:%02d:%02d",
	    time->tm_year+1900, time->tm_mon+1, time->tm_mday,
	    time->tm_hour, time->tm_min, time->tm_sec);

    return result;
}

time_t
now(void)
{
    time_t sec;

    time(&sec);
    return(sec);
}

bool
timestampeq(time_t t1, time_t t2)
{
    return difftime(t1, t2) == 0;
}

bool
timestampne(time_t t1, time_t t2)
{
    return difftime(t1, t2) != 0;
}

bool
timestamplt(time_t t1, time_t t2)
{
    return difftime(t1, t2) > 0;
}

bool
timestampgt(time_t t1, time_t t2)
{
    return difftime(t1, t2) < 0;
}

bool
timestample(time_t t1, time_t t2)
{
    return difftime(t1, t2) >= 0;
}

bool
timestampge(time_t t1, time_t t2)
{
    return difftime(t1, t2) <= 0;
}

DateTime *
timestamp_datetime(time_t timestamp)
{
    DateTime *result;

    double fsec = 0;
    struct tm *tm;

    if (!PointerIsValid(result = PALLOCTYPE(DateTime)))
	elog(WARN,"Memory allocation failed, can't convert timestamp to datetime",NULL);

    tm = localtime((time_t *) &timestamp);
    tm->tm_year += 1900;
    tm->tm_mon += 1;

    *result = tm2datetime(tm, fsec, NULL);

    return(result);
} /* timestamp_datetime() */
