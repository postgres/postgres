/*-------------------------------------------------------------------------
 *
 * port.c--
 *	  Intel x86/Intel SVR4-specific routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  /usr/local/devel/pglite/cvs/src/backend/port/svr4/port.c,v 1.2 1995/03/17 06:40:19 andrew Exp
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>				/* for pow() prototype */

#include <errno.h>
#include "rusagestub.h"
#include "port-protos.h"

long
random()
{
	return (lrand48());
}

void
srandom(int seed)
{
	srand48((long int) seed);
}

int
getrusage(int who, struct rusage * rusage)
{
	struct tms	tms;
	register int tick_rate = CLK_TCK;	/* ticks per second */
	clock_t		u,
				s;

	if (rusage == (struct rusage *) NULL)
	{
		errno = EFAULT;
		return (-1);
	}
	if (times(&tms) < 0)
	{
		/* errno set by times */
		return (-1);
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
			return (-1);
	}
#define TICK_TO_SEC(T, RATE)	((T)/(RATE))
#define TICK_TO_USEC(T,RATE)	(((T)%(RATE)*1000000)/RATE)
	rusage->ru_utime.tv_sec = TICK_TO_SEC(u, tick_rate);
	rusage->ru_utime.tv_usec = TICK_TO_USEC(u, tick_rate);
	rusage->ru_stime.tv_sec = TICK_TO_SEC(s, tick_rate);
	rusage->ru_stime.tv_usec = TICK_TO_USEC(u, tick_rate);
	return (0);
}

/*
 * Copyright (c) 1987 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of California at Berkeley. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific written prior permission. This software
 * is provided ``as is'' without express or implied warranty.
 */

#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)strcasecmp.c	5.5 (Berkeley) 11/24/87";

#endif							/* LIBC_SCCS and not lint */

#include <sys/types.h>
#include <string.h>

#include <sys/utsname.h>

int
gethostname(char *name, int namelen)
{
	static struct utsname mname;
	static int	called = 0;

	if (!called)
	{
		called++;
		uname(&mname);
	}
	strncpy(name, mname.nodename, (SYS_NMLN < namelen ? SYS_NMLN : namelen));

	return (0);
}
