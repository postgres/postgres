/*-------------------------------------------------------------------------
 *
 * win32common.c
 *	  Common routines shared among the win32*.c ports.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/win32common.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef FRONTEND
#include "postgres_fe.h"
#else
#include "postgres.h"
#endif

#ifdef WIN32

/*
 * pgwin32_get_file_type
 *
 * Convenience wrapper for GetFileType() with specific error handling for all the
 * port implementations.  Returns the file type associated with a HANDLE.
 *
 * On error, sets errno with FILE_TYPE_UNKNOWN as file type.
 */
DWORD
pgwin32_get_file_type(HANDLE hFile)
{
	DWORD		fileType = FILE_TYPE_UNKNOWN;
	DWORD		lastError;

	errno = 0;

	/*
	 * When stdin, stdout, and stderr aren't associated with a stream the
	 * special value -2 is returned:
	 * https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/get-osfhandle
	 */
	if (hFile == INVALID_HANDLE_VALUE || hFile == (HANDLE) -2)
	{
		errno = EINVAL;
		return FILE_TYPE_UNKNOWN;
	}

	fileType = GetFileType(hFile);
	lastError = GetLastError();

	/*
	 * Invoke GetLastError in order to distinguish between a "valid" return of
	 * FILE_TYPE_UNKNOWN and its return due to a calling error.  In case of
	 * success, GetLastError() returns NO_ERROR.
	 */
	if (fileType == FILE_TYPE_UNKNOWN && lastError != NO_ERROR)
	{
		_dosmaperr(lastError);
		return FILE_TYPE_UNKNOWN;
	}

	return fileType;
}

#endif							/* WIN32 */
