/*-------------------------------------------------------------------------
 *
 * noblock.c
 *	  set a file descriptor as non-blocking
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/noblock.c,v 1.5 2004/12/31 22:03:53 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <sys/types.h>
#include <fcntl.h>

bool
set_noblock(int sock)
{
#if !defined(WIN32) && !defined(__BEOS__)
	return (fcntl(sock, F_SETFL, O_NONBLOCK) != -1);
#else
	long		ioctlsocket_ret = 1;

	/* Returns non-0 on failure, while fcntl() returns -1 on failure */
#ifdef WIN32
	return (ioctlsocket(sock, FIONBIO, &ioctlsocket_ret) == 0);
#endif
#ifdef __BEOS__
	return (ioctl(sock, FIONBIO, &ioctlsocket_ret) == 0);
#endif
#endif
}
