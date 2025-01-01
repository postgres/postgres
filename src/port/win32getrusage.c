/*-------------------------------------------------------------------------
 *
 * win32getrusage.c
 *	  get information about resource utilisation
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/win32getrusage.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <sys/resource.h>

int
getrusage(int who, struct rusage *rusage)
{
	FILETIME	starttime;
	FILETIME	exittime;
	FILETIME	kerneltime;
	FILETIME	usertime;
	ULARGE_INTEGER li;

	if (who != RUSAGE_SELF)
	{
		/* Only RUSAGE_SELF is supported in this implementation for now */
		errno = EINVAL;
		return -1;
	}

	if (rusage == (struct rusage *) NULL)
	{
		errno = EFAULT;
		return -1;
	}
	memset(rusage, 0, sizeof(struct rusage));
	if (GetProcessTimes(GetCurrentProcess(),
						&starttime, &exittime, &kerneltime, &usertime) == 0)
	{
		_dosmaperr(GetLastError());
		return -1;
	}

	/* Convert FILETIMEs (0.1 us) to struct timeval */
	memcpy(&li, &kerneltime, sizeof(FILETIME));
	li.QuadPart /= 10L;			/* Convert to microseconds */
	rusage->ru_stime.tv_sec = li.QuadPart / 1000000L;
	rusage->ru_stime.tv_usec = li.QuadPart % 1000000L;

	memcpy(&li, &usertime, sizeof(FILETIME));
	li.QuadPart /= 10L;			/* Convert to microseconds */
	rusage->ru_utime.tv_sec = li.QuadPart / 1000000L;
	rusage->ru_utime.tv_usec = li.QuadPart % 1000000L;

	return 0;
}
