/*-------------------------------------------------------------------------
 *
 * win32link.c
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
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
		/*
		 * CreateHardLinkA reports ERROR_INVALID_FUNCTION if the target file
		 * is on a filesystem that doesn't support hard links.  _dosmaperr
		 * would map that to EINVAL by default, but we want to report ENOTSUP
		 * because that will cause zic.c to fall back to making copies.
		 * However, EINVAL is probably the best translation in most cases, so
		 * tweak it here rather than changing _dosmaperr's behavior.
		 */
		DWORD		error = GetLastError();

		if (error == ERROR_INVALID_FUNCTION)
			errno = ENOTSUP;
		else
			_dosmaperr(error);
		return -1;
	}
	else
		return 0;
}
