/* $Id: gethostname.c,v 1.3 1998/01/15 20:54:34 scrappy Exp $ */

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

	return (0);
}
