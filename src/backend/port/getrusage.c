/* $Id: getrusage.c,v 1.4 1998/02/01 00:02:59 scrappy Exp $ */

#include <math.h>               /* for pow() prototype */

#include <errno.h>
#include "rusagestub.h"
#include "port-protos.h"

#if 0 /* this is from univel port ... how does compiler define? */
      /* same for i386_solaris port ... how does compiler define? */
      /* same for sco port ... how does compiler define? */
      /* same for sparc_solaris port ... how does compiler define? */
      /* same for svr4 port ... how does compiler define? */
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
#endif

#if 0 /* this is for hpux port ... how does compiler define? */
getrusage(int who, struct rusage * ru)
{
    return (syscall(SYS_GETRUSAGE, who, ru));
}
#endif
