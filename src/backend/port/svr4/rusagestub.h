/*-------------------------------------------------------------------------
 *
 * rusagestub.h--
 *	  Stubs for getrusage(3).
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * rusagestub.h,v 1.1.1.1 1994/11/07 05:19:39 andrew Exp
 *
 *-------------------------------------------------------------------------
 */
#ifndef RUSAGESTUB_H
#define RUSAGESTUB_H

#include <sys/time.h>			/* for struct timeval */
#include <sys/times.h>			/* for struct tms */
#include <limits.h>				/* for CLK_TCK */

#define RUSAGE_SELF		0
#define RUSAGE_CHILDREN -1

struct rusage
{
	struct timeval	ru_utime;	/* user time used */
	struct timeval	ru_stime;	/* system time used */
};

extern int		getrusage(int who, struct rusage * rusage);

#endif							/* RUSAGESTUB_H */
