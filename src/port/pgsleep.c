/*-------------------------------------------------------------------------
 *
 * pgsleep.c
 *	   Portable delay handling.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * src/port/pgsleep.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <time.h>

/*
 * In a Windows backend, we don't use this implementation, but rather
 * the signal-aware version in src/backend/port/win32/signal.c.
 */
#if defined(FRONTEND) || !defined(WIN32)

/*
 * pg_usleep --- delay the specified number of microseconds.
 *
 * NOTE: Although the delay is specified in microseconds, older Unixen and
 * Windows use periodic kernel ticks to wake up, which might increase the delay
 * time significantly.  We've observed delay increases as large as 20
 * milliseconds on supported platforms.
 *
 * On machines where "long" is 32 bits, the maximum delay is ~2000 seconds.
 *
 * CAUTION: It's not a good idea to use long sleeps in the backend.  They will
 * silently return early if a signal is caught, but that doesn't include
 * latches being set on most OSes, and even signal handlers that set MyLatch
 * might happen to run before the sleep begins, allowing the full delay.
 * Better practice is to use WaitLatch() with a timeout, so that backends
 * respond to latches and signals promptly.
 */
void
pg_usleep(long microsec)
{
	if (microsec > 0)
	{
#ifndef WIN32
		struct timespec delay;

		delay.tv_sec = microsec / 1000000L;
		delay.tv_nsec = (microsec % 1000000L) * 1000;
		(void) nanosleep(&delay, NULL);
#else
		SleepEx((microsec < 500 ? 1 : (microsec + 500) / 1000), FALSE);
#endif
	}
}

#endif							/* defined(FRONTEND) || !defined(WIN32) */
