/*-------------------------------------------------------------------------
 *
 * timer.c
 *	  Microsoft Windows Win32 Timer Implementation
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/win32/timer.c,v 1.4 2004/08/29 05:06:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqsignal.h"


static HANDLE timerHandle = INVALID_HANDLE_VALUE;

static VOID CALLBACK
timer_completion(LPVOID arg, DWORD timeLow, DWORD timeHigh)
{
	pg_queue_signal(SIGALRM);
}


/*
 * Limitations of this implementation:
 *
 * - Does not support setting ovalue
 * - Does not support interval timer (value->it_interval)
 * - Only supports ITIMER_REAL
 */
int
setitimer(int which, const struct itimerval * value, struct itimerval * ovalue)
{
	LARGE_INTEGER dueTime;

	Assert(ovalue == NULL);
	Assert(value != NULL);
	Assert(value->it_interval.tv_sec == 0 && value->it_interval.tv_usec == 0);
	Assert(which == ITIMER_REAL);

	if (timerHandle == INVALID_HANDLE_VALUE)
	{
		/* First call in this backend, create new timer object */
		timerHandle = CreateWaitableTimer(NULL, TRUE, NULL);
		if (timerHandle == NULL)
			ereport(FATAL,
					(errmsg_internal("failed to create waitable timer: %i", (int) GetLastError())));
	}

	if (value->it_value.tv_sec == 0 &&
		value->it_value.tv_usec == 0)
	{
		/* Turn timer off */
		CancelWaitableTimer(timerHandle);
		return 0;
	}

	/* Negative time to SetWaitableTimer means relative time */
	dueTime.QuadPart = -(value->it_value.tv_usec * 10 + value->it_value.tv_sec * 10000000L);

	/* Turn timer on, or change timer */
	if (!SetWaitableTimer(timerHandle, &dueTime, 0, timer_completion, NULL, FALSE))
		ereport(FATAL,
				(errmsg_internal("failed to set waitable timer: %i", (int) GetLastError())));

	return 0;
}
