/* $PostgreSQL: pgsql/src/port/strerror.c,v 1.4 2005/02/22 04:43:16 momjian Exp $ */

/*
 * strerror - map error number to descriptive string
 *
 * This version is obviously somewhat Unix-specific.
 *
 * based on code by Henry Spencer
 * modified for ANSI by D'Arcy J.M. Cain
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>

extern const char *const sys_errlist[];
extern int	sys_nerr;

const char *
strerror(int errnum)
{
	static char buf[24];

	if (errnum < 0 || errnum > sys_nerr)
	{
		sprintf(buf, _("unrecognized error %d"), errnum);
		return buf;
	}

	return sys_errlist[errnum];
}
