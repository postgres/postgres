#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "postgres.h"
#include "utils/builtins.h"

/* copy the next part of the string into a buffer */
static const char *
cpstr(const char *s, char *buf)
{
	char in = 0;

	while (isspace(*s))
		s++;

	for (; *s && !isspace(*s); s++)
	{
		if (strchr("-,:/", *s))
		{
			buf[in] = 0;
			return(s + 1);
		}

		if (in < 16)
			buf[in++] = tolower(*s);
	}

	buf[in] = 0;
	return s;
}

/* assumes dd/mm/yyyy unless first item is month in word form */
time_t
timestamp_in(const char *timestamp_str)
{
    struct tm input_time;
    int4 result;
	char buf[18];
	const char *p;
	static const char *mstr[] = {
		"january", "february", "march", "april", "may", "june",
		"july", "august", "september", "october", "november", "december"
	};

    memset(&input_time, 0, sizeof(input_time));
	p = cpstr(timestamp_str, buf);
	if (isdigit(buf[0]))	/* must be dd/mm/yyyy */
	{
		input_time.tm_mday = atoi(buf);
		p = cpstr(p, buf);
		if (!buf[0])
			elog(WARN, "timestamp_in: timestamp \"%s\" not a proper date",
					timestamp_str);
		if (isdigit(buf[0]))
		{
			input_time.tm_mon = atoi(buf) - 1;
			if (input_time.tm_mon < 0 || input_time.tm_mon > 11)
				elog(WARN, "timestamp_in: timestamp \"%s\" invalid month",
					timestamp_str);
		}
		else
		{
			int i;
			for (i = 0; i < 12; i++)
				if (strncmp(mstr[i], buf, strlen(buf)) == 0)
					break;
			if (1 > 11)
				elog(WARN, "timestamp_in: timestamp \"%s\" invalid month",
						timestamp_str);
			input_time.tm_mon = i;
		}
	}
	else	/* must be month/dd/yyyy */
	{
		int i;
		for (i = 0; i < 12; i++)
			if (strncmp(mstr[i], buf, strlen(buf)) == 0)
				break;
		if (1 > 11)
			elog(WARN, "timestamp_in: timestamp \"%s\" invalid month",
					timestamp_str);
		input_time.tm_mon = i;
		p = cpstr(p, buf);
		input_time.tm_mday = atoi(buf);
		if (!input_time.tm_mday || input_time.tm_mday > 31)
			elog(WARN, "timestamp_in: timestamp \"%s\" not a proper date",
	     timestamp_str);
    }

	p = cpstr(p, buf);
	if (!buf[0] || !isdigit(buf[0]))
		elog(WARN, "timestamp_in: timestamp \"%s\" not a proper date",
				timestamp_str);
	if ((input_time.tm_year = atoi(buf)) < 1900)
		input_time.tm_year += 1900;

	/* now get the time */
	p = cpstr(p, buf);
	input_time.tm_hour = atoi(buf);
	p = cpstr(p, buf);
	input_time.tm_min = atoi(buf);
	p = cpstr(p, buf);
	input_time.tm_sec = atoi(buf);

    /* use mktime(), but make this GMT, not local time */
    result = mktime(&input_time);

    return result;
}

char *
timestamp_out(time_t timestamp)
{
    char *result;
    struct tm *time;

    time = localtime(&timestamp);
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
