/*-------------------------------------------------------------------------
 *
 * win32link.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/win32link.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

int
link(const char *src, const char *dst)
{
	/*
	 * CreateHardLinkA returns zero for failure
	 * https://docs.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createhardlinka
	 */
	if (CreateHardLinkA(dst, src, NULL) == 0)
	{
		_dosmaperr(GetLastError());
		return -1;
	}
	else
		return 0;
}
