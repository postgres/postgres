/* $Id: gethostname.c,v 1.4 1998/09/01 03:24:25 momjian Exp $ */

#include <sys/types.h>
#include <string.h>

#include <sys/utsname.h>

#include "config.h"

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
