/*-------------------------------------------------------------------------
 *
 * noblock.c
 *	  set a file descriptor as non-blocking
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/noblock.c,v 1.10 2006/03/05 15:59:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <fcntl.h>


bool
pg_set_noblock(int sock)
{
#if !defined(WIN32)
	return (fcntl(sock, F_SETFL, O_NONBLOCK) != -1);
#else
	long		ioctlsocket_ret = 1;

	/* Returns non-0 on failure, while fcntl() returns -1 on failure */
	return (ioctlsocket(sock, FIONBIO, &ioctlsocket_ret) == 0);
#endif
}


bool
pg_set_block(int sock)
{
#if !defined(WIN32)
	int			flags;

	flags = fcntl(sock, F_GETFL);
	if (flags < 0 || fcntl(sock, F_SETFL, (long) (flags & ~O_NONBLOCK)))
		return false;
	return true;
#else
	long		ioctlsocket_ret = 0;

	/* Returns non-0 on failure, while fcntl() returns -1 on failure */
	return (ioctlsocket(sock, FIONBIO, &ioctlsocket_ret) == 0);
#endif
}
