/*-------------------------------------------------------------------------
 *
 * port.c--
 *    SCO 3.2v5.0.2 specific routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    /usr/local/devel/pglite/cvs/src/backend/port/svr4/port.c,v 1.2 1995/03/17 06:40:19 andrew Exp
 *
 *-------------------------------------------------------------------------
 */
#include <unistd.h>
#include <math.h>		/* for pow() prototype */

#include <errno.h>
#include "rusagestub.h"


int
getrusage(int who, struct rusage *rusage)
{
    struct tms tms;
    register int tick_rate = CLK_TCK;	/* ticks per second */
    clock_t u, s;

    if (rusage == (struct rusage *) NULL) {
	errno = EFAULT;
	return(-1);
    }
    if (times(&tms) < 0) {
	/* errno set by times */
	return(-1);
    }
    switch (who) {
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
	return(-1);
    }
#define TICK_TO_SEC(T, RATE)	((T)/(RATE))
#define	TICK_TO_USEC(T,RATE)	(((T)%(RATE)*1000000)/RATE)
    rusage->ru_utime.tv_sec = TICK_TO_SEC(u, tick_rate);
    rusage->ru_utime.tv_usec = TICK_TO_USEC(u, tick_rate);
    rusage->ru_stime.tv_sec = TICK_TO_SEC(s, tick_rate);
    rusage->ru_stime.tv_usec = TICK_TO_USEC(u, tick_rate);
    return(0);
}

