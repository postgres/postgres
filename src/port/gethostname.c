/*-------------------------------------------------------------------------
 *
 * gethostname.c
 *	  gethostname using uname
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/port/gethostname.c,v 1.3 2003/11/11 23:52:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

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

	return 0;
}
