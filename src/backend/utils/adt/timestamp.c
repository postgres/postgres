#include <stdio.h>
#include <string.h>
#include <time.h>
#include "postgres.h"
#include "utils/builtins.h"

int4
timestamp_in(char *timestamp_str)
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
    result -= timezone;

    return result;
}

char *
timestamp_out(int4 timestamp)
{
    char *result;
    struct tm *time;

    time = gmtime((time_t *)&timestamp);
    result = palloc(20);
    sprintf(result, "%04d-%02d-%02d %02d:%02d:%02d",
	    time->tm_year+1900, time->tm_mon+1, time->tm_mday,
	    time->tm_hour, time->tm_min, time->tm_sec);

    return result;
}

int4
now(void)
{
    struct tm ignore;
    time_t sec;

    /* we want the local time here.  but 'timezone' doesn't get set */
    /* until we do a mktime().  so do one.                          */
    memset(&ignore, 0, sizeof(ignore));
    mktime(&ignore);

    time(&sec);
    sec -= timezone;
    return((int4)sec);
}

int4
timestampeq(int4 t1, int4 t2)
{
    return t1 == t2;
}

int4
timestampne(int4 t1, int4 t2)
{
    return t1 != t2;
}

int4
timestamplt(int4 t1, int4 t2)
{
    return t1 < t2;
}

int4
timestampgt(int4 t1, int4 t2)
{
    return t1 > t2;
}

int4
timestample(int4 t1, int4 t2)
{
    return t1 <= t2;
}

int4
timestampge(int4 t1, int4 t2)
{
    return t1 >= t2;
}
