/*-------------------------------------------------------------------------
 *
 * rusagestub.h
 *	  Stubs for getrusage(3).
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rusagestub.h,v 1.4 1999/02/13 23:20:47 momjian Exp $
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
	struct timeval ru_utime;	/* user time used */
	struct timeval ru_stime;	/* system time used */
};

extern int	getrusage(int who, struct rusage * rusage);

#endif	 /* RUSAGESTUB_H */
