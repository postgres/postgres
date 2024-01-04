/*-------------------------------------------------------------------------
 *
 * win32fdatasync.c
 *	   Win32 fdatasync() replacement
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/port/win32fdatasync.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef FRONTEND
#include "postgres_fe.h"
#else
#include "postgres.h"
#endif

#include "port/win32ntdll.h"

int
fdatasync(int fd)
{
	IO_STATUS_BLOCK iosb;
	NTSTATUS	status;
	HANDLE		handle;

	handle = (HANDLE) _get_osfhandle(fd);
	if (handle == INVALID_HANDLE_VALUE)
	{
		errno = EBADF;
		return -1;
	}

	if (initialize_ntdll() < 0)
		return -1;

	memset(&iosb, 0, sizeof(iosb));
	status = pg_NtFlushBuffersFileEx(handle,
									 FLUSH_FLAGS_FILE_DATA_SYNC_ONLY,
									 NULL,
									 0,
									 &iosb);

	if (NT_SUCCESS(status))
		return 0;

	_dosmaperr(pg_RtlNtStatusToDosError(status));
	return -1;
}
