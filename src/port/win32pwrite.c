/*-------------------------------------------------------------------------
 *
 * win32pwrite.c
 *	  Implementation of pwrite(2) for Windows.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/win32pwrite.c
 *
 *-------------------------------------------------------------------------
 */


#include "c.h"

#include <windows.h>

ssize_t
pg_pwrite(int fd, const void *buf, size_t size, off_t offset)
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

	/* Avoid overflowing DWORD. */
	size = Min(size, 1024 * 1024 * 1024);

	/* Note that this changes the file position, despite not using it. */
	overlapped.Offset = offset;
	if (!WriteFile(handle, buf, size, &result, &overlapped))
	{
		_dosmaperr(GetLastError());
		return -1;
	}

	return result;
}
