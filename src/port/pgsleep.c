/*-------------------------------------------------------------------------
 *
 * pgsleep.c
 *	   Portable delay handling.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/port/pgsleep.c,v 1.6 2004/12/31 22:03:53 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <unistd.h>
#include <sys/time.h>

/*
 * pg_usleep --- delay the specified number of microseconds.
 *
 * NOTE: although the delay is specified in microseconds, the effective
 * resolution is only 1/HZ, or 10 milliseconds, on most Unixen.  Expect
 * the requested delay to be rounded up to the next resolution boundary.
 *
 * On machines where "long" is 32 bits, the maximum delay is ~2000 seconds.
 */
#ifdef pg_usleep
#undef pg_usleep
#endif
void
pg_usleep(long microsec)
{
	if (microsec > 0)
	{
#ifndef WIN32
		struct timeval delay;

		delay.tv_sec = microsec / 1000000L;
		delay.tv_usec = microsec % 1000000L;
		(void) select(0, NULL, NULL, NULL, &delay);
#else
		SleepEx((microsec < 500 ? 1 : (microsec + 500) / 1000), FALSE);
#endif
	}
}
