/*-------------------------------------------------------------------------
 *
 * port.c--
 *    Intel x86/Intel SVR4-specific routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    /usr/local/devel/pglite/cvs/src/backend/port/svr4/port.c,v 1.2 1995/03/17 06:40:19 andrew Exp
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>		/* for pow() prototype */

#include <errno.h>
#include "rusagestub.h"

long
random()
{
    return(lrand48());
}

void
srandom(int seed)
{
    srand48((long int) seed);
}

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
#endif /* LIBC_SCCS and not lint */

#include <sys/types.h>
#include <string.h>

/*
 * This array is designed for mapping upper and lower case letter
 * together for a case independent comparison.  The mappings are
p * based upon ascii character sequences.
 */
static unsigned char charmap[] = {
	'\000', '\001', '\002', '\003', '\004', '\005', '\006', '\007',
	'\010', '\011', '\012', '\013', '\014', '\015', '\016', '\017',
	'\020', '\021', '\022', '\023', '\024', '\025', '\026', '\027',
	'\030', '\031', '\032', '\033', '\034', '\035', '\036', '\037',
	'\040', '\041', '\042', '\043', '\044', '\045', '\046', '\047',
	'\050', '\051', '\052', '\053', '\054', '\055', '\056', '\057',
	'\060', '\061', '\062', '\063', '\064', '\065', '\066', '\067',
	'\070', '\071', '\072', '\073', '\074', '\075', '\076', '\077',
	'\100', '\141', '\142', '\143', '\144', '\145', '\146', '\147',
	'\150', '\151', '\152', '\153', '\154', '\155', '\156', '\157',
	'\160', '\161', '\162', '\163', '\164', '\165', '\166', '\167',
	'\170', '\171', '\172', '\133', '\134', '\135', '\136', '\137',
	'\140', '\141', '\142', '\143', '\144', '\145', '\146', '\147',
	'\150', '\151', '\152', '\153', '\154', '\155', '\156', '\157',
	'\160', '\161', '\162', '\163', '\164', '\165', '\166', '\167',
	'\170', '\171', '\172', '\173', '\174', '\175', '\176', '\177',
	'\200', '\201', '\202', '\203', '\204', '\205', '\206', '\207',
	'\210', '\211', '\212', '\213', '\214', '\215', '\216', '\217',
	'\220', '\221', '\222', '\223', '\224', '\225', '\226', '\227',
	'\230', '\231', '\232', '\233', '\234', '\235', '\236', '\237',
	'\240', '\241', '\242', '\243', '\244', '\245', '\246', '\247',
	'\250', '\251', '\252', '\253', '\254', '\255', '\256', '\257',
	'\260', '\261', '\262', '\263', '\264', '\265', '\266', '\267',
	'\270', '\271', '\272', '\273', '\274', '\275', '\276', '\277',
	'\300', '\341', '\342', '\343', '\344', '\345', '\346', '\347',
	'\350', '\351', '\352', '\353', '\354', '\355', '\356', '\357',
	'\360', '\361', '\362', '\363', '\364', '\365', '\366', '\367',
	'\370', '\371', '\372', '\333', '\334', '\335', '\336', '\337',
	'\340', '\341', '\342', '\343', '\344', '\345', '\346', '\347',
	'\350', '\351', '\352', '\353', '\354', '\355', '\356', '\357',
	'\360', '\361', '\362', '\363', '\364', '\365', '\366', '\367',
	'\370', '\371', '\372', '\373', '\374', '\375', '\376', '\377',
};

int
strcasecmp(char *s1, char *s2)
{
    register unsigned char u1, u2;

    for (;;) {
		u1 = (unsigned char) *s1++;
		u2 = (unsigned char) *s2++;
		if (charmap[u1] != charmap[u2]) {
			return charmap[u1] - charmap[u2];
		}
		if (u1 == '\0') {
			return 0;
		}
    }
}

