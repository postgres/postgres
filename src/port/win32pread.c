/*-------------------------------------------------------------------------
 *
 * win32pread.c
 *	  Implementation of pread(2) for Windows.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/win32pread.c
 *
 *-------------------------------------------------------------------------
 */


#include "c.h"

#include <windows.h>

ssize_t
pg_pread(int fd, void *buf, size_t size, off_t offset)
{
	OVERLAPPED	overlapped = {0};
	HANDLE		handle;
	DWORD		result;

	handle = (HANDLE) _get_osfhandle(fd);
	if (handle == INVALID_HANDLE_VALUE)
	{
		errno = EBADF;
		return -1;
	}

	/* Note that this changes the file position, despite not using it. */
	overlapped.Offset = offset;
	if (!ReadFile(handle, buf, size, &result, &overlapped))
	{
		if (GetLastError() == ERROR_HANDLE_EOF)
			return 0;

		_dosmaperr(GetLastError());
		return -1;
	}

	return result;
}
