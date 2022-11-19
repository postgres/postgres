/*
 * Replacement for <sys/resource.h> for Windows.
 */
#ifndef WIN32_SYS_RESOURCE_H
#define WIN32_SYS_RESOURCE_H

#include <sys/time.h>			/* for struct timeval */

#define RUSAGE_SELF		0
#define RUSAGE_CHILDREN (-1)

struct rusage
{
	struct timeval ru_utime;	/* user time used */
	struct timeval ru_stime;	/* system time used */
};

extern int	getrusage(int who, struct rusage *rusage);

#endif							/* WIN32_SYS_RESOURCE_H */
