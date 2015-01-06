/*-------------------------------------------------------------------------
 *
 * getrusage.c
 *	  get information about resource utilisation
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/getrusage.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include "rusagestub.h"

/* This code works on:
 *		sco
 *		solaris_i386
 *		solaris_sparc
 *		hpux 9.*
 *		win32
 * which currently is all the supported platforms that don't have a
 * native version of getrusage().  So, if configure decides to compile
 * this file at all, we just use this version unconditionally.
 */

int
getrusage(int who, struct rusage * rusage)
{
#ifdef WIN32
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
#else							/* all but WIN32 */

	struct tms	tms;
	int			tick_rate = CLK_TCK;	/* ticks per second */
	clock_t		u,
				s;

	if (rusage == (struct rusage *) NULL)
	{
		errno = EFAULT;
		return -1;
	}
	if (times(&tms) < 0)
	{
		/* errno set by times */
		return -1;
	}
	switch (who)
	{
		case RUSAGE_SELF:
			u = tms.tms_utime;
			s = tms.tms_stime;
			break;
		case RUSAGE_CHILDREN:
			u = tms.tms_cutime;
			s = tms.tms_cstime;
			break;
		default:
			errno = EINVAL;
			return -1;
	}
#define TICK_TO_SEC(T, RATE)	((T)/(RATE))
#define TICK_TO_USEC(T,RATE)	(((T)%(RATE)*1000000)/RATE)
	rusage->ru_utime.tv_sec = TICK_TO_SEC(u, tick_rate);
	rusage->ru_utime.tv_usec = TICK_TO_USEC(u, tick_rate);
	rusage->ru_stime.tv_sec = TICK_TO_SEC(s, tick_rate);
	rusage->ru_stime.tv_usec = TICK_TO_USEC(u, tick_rate);
#endif   /* WIN32 */

	return 0;
}
