/* $Id: gethostname.c,v 1.5 2001/08/24 14:07:49 petere Exp $ */

#include "c.h"

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

	return 0;
}
