/*-------------------------------------------------------------------------
 *
 * kill.c
 *	  kill()
 *
 * Copyright (c) 1996-2006, PostgreSQL Global Development Group
 *
 *	This is a replacement version of kill for Win32 which sends
 *	signals that the backend can recognize.
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/kill.c,v 1.7 2006/03/05 15:59:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#ifdef WIN32
/* signal sending */
int
pgkill(int pid, int sig)
{
	char		pipename[128];
	BYTE		sigData = sig;
	BYTE		sigRet = 0;
	DWORD		bytes;

	/* we allow signal 0 here, but it will be ignored in pg_queue_signal */
	if (sig >= PG_SIGNAL_COUNT || sig < 0)
	{
		errno = EINVAL;
		return -1;
	}
	if (pid <= 0)
	{
		/* No support for process groups */
		errno = EINVAL;
		return -1;
	}
	wsprintf(pipename, "\\\\.\\pipe\\pgsignal_%i", pid);
	if (!CallNamedPipe(pipename, &sigData, 1, &sigRet, 1, &bytes, 1000))
	{
		if (GetLastError() == ERROR_FILE_NOT_FOUND)
			errno = ESRCH;
		else if (GetLastError() == ERROR_ACCESS_DENIED)
			errno = EPERM;
		else
			errno = EINVAL;
		return -1;
	}
	if (bytes != 1 || sigRet != sig)
	{
		errno = ESRCH;
		return -1;
	}

	return 0;
}

#endif
