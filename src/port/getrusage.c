/* $Id: getrusage.c,v 1.2 2003/05/15 16:35:30 momjian Exp $ */

#include <stdio.h>
#include <errno.h>

#include "postgres.h"
#include "rusagestub.h"

/* This code works on:
 *		univel
 *		solaris_i386
 *		sco
 *		solaris_sparc
 *		svr4
 *		hpux 9.*
 * which currently is all the supported platforms that don't have a
 * native version of getrusage().  So, if configure decides to compile
 * this file at all, we just use this version unconditionally.
 */

int
getrusage(int who, struct rusage * rusage)
{
#ifdef WIN32
	if (rusage)
		memset(rusage, 0, sizeof(rusage));
#else
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
#endif
	return 0;
}
