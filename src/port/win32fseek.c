/*-------------------------------------------------------------------------
 *
 * win32fseek.c
 *	  Replacements for fseeko() and ftello().
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/win32fseek.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef FRONTEND
#include "postgres_fe.h"
#else
#include "postgres.h"
#endif

#ifdef _MSC_VER

/*
 * _pgfseeko64
 *
 * Calling fseek() on a handle to a non-seeking device such as a pipe or
 * a communications device is not supported, and fseek() may not return
 * an error.  This wrapper relies on the file type to check which cases
 * are supported.
 */
int
_pgfseeko64(FILE *stream, pgoff_t offset, int origin)
{
	DWORD		fileType;
	HANDLE		hFile = (HANDLE) _get_osfhandle(_fileno(stream));

	fileType = pgwin32_get_file_type(hFile);
	if (errno != 0)
		return -1;

	if (fileType == FILE_TYPE_DISK)
		return _fseeki64(stream, offset, origin);
	else if (fileType == FILE_TYPE_CHAR || fileType == FILE_TYPE_PIPE)
		errno = ESPIPE;
	else
		errno = EINVAL;

	return -1;
}

/*
 * _pgftello64
 *
 * Same as _pgfseeko64().
 */
pgoff_t
_pgftello64(FILE *stream)
{
	DWORD		fileType;
	HANDLE		hFile = (HANDLE) _get_osfhandle(_fileno(stream));

	fileType = pgwin32_get_file_type(hFile);
	if (errno != 0)
		return -1;

	if (fileType == FILE_TYPE_DISK)
		return _ftelli64(stream);
	else if (fileType == FILE_TYPE_CHAR || fileType == FILE_TYPE_PIPE)
		errno = ESPIPE;
	else
		errno = EINVAL;

	return -1;
}

#endif							/* _MSC_VER */
