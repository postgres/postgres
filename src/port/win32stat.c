/*-------------------------------------------------------------------------
 *
 * win32stat.c
 *	  Replacements for <sys/stat.h> functions using GetFileInformationByHandle
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/win32stat.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef WIN32

#include "c.h"
#include <windows.h>

/*
 * Convert a FILETIME struct into a 64 bit time_t.
 */
static __time64_t
filetime_to_time(const FILETIME *ft)
{
	ULARGE_INTEGER unified_ft = {0};
	static const uint64 EpochShift = UINT64CONST(116444736000000000);

	unified_ft.LowPart = ft->dwLowDateTime;
	unified_ft.HighPart = ft->dwHighDateTime;

	if (unified_ft.QuadPart < EpochShift)
		return -1;

	unified_ft.QuadPart -= EpochShift;
	unified_ft.QuadPart /= 10 * 1000 * 1000;

	return unified_ft.QuadPart;
}

/*
 * Convert WIN32 file attributes to a Unix-style mode.
 *
 * Only owner permissions are set.
 */
static unsigned short
fileattr_to_unixmode(int attr)
{
	unsigned short uxmode = 0;

	uxmode |= (unsigned short) ((attr & FILE_ATTRIBUTE_DIRECTORY) ?
								(_S_IFDIR) : (_S_IFREG));

	uxmode |= (unsigned short) ((attr & FILE_ATTRIBUTE_READONLY) ?
								(_S_IREAD) : (_S_IREAD | _S_IWRITE));

	/* there is no need to simulate _S_IEXEC using CMD's PATHEXT extensions */
	uxmode |= _S_IEXEC;

	return uxmode;
}

/*
 * Convert WIN32 file information (from a HANDLE) to a struct stat.
 */
static int
fileinfo_to_stat(HANDLE hFile, struct stat *buf)
{
	BY_HANDLE_FILE_INFORMATION fiData;

	memset(buf, 0, sizeof(*buf));

	/*
	 * GetFileInformationByHandle minimum supported version: Windows XP and
	 * Windows Server 2003, so it exists everywhere we care about.
	 */
	if (!GetFileInformationByHandle(hFile, &fiData))
	{
		_dosmaperr(GetLastError());
		return -1;
	}

	if (fiData.ftLastWriteTime.dwLowDateTime ||
		fiData.ftLastWriteTime.dwHighDateTime)
		buf->st_mtime = filetime_to_time(&fiData.ftLastWriteTime);

	if (fiData.ftLastAccessTime.dwLowDateTime ||
		fiData.ftLastAccessTime.dwHighDateTime)
		buf->st_atime = filetime_to_time(&fiData.ftLastAccessTime);
	else
		buf->st_atime = buf->st_mtime;

	if (fiData.ftCreationTime.dwLowDateTime ||
		fiData.ftCreationTime.dwHighDateTime)
		buf->st_ctime = filetime_to_time(&fiData.ftCreationTime);
	else
		buf->st_ctime = buf->st_mtime;

	buf->st_mode = fileattr_to_unixmode(fiData.dwFileAttributes);
	buf->st_nlink = fiData.nNumberOfLinks;

	buf->st_size = ((((uint64) fiData.nFileSizeHigh) << 32) |
					fiData.nFileSizeLow);

	return 0;
}

/*
 * Windows implementation of stat().
 *
 * This currently also implements lstat(), though perhaps that should change.
 */
int
_pgstat64(const char *name, struct stat *buf)
{
	/*
	 * Our open wrapper will report STATUS_DELETE_PENDING as ENOENT.  We
	 * request FILE_FLAG_BACKUP_SEMANTICS so that we can open directories too,
	 * for limited purposes.  We use the private handle-based version, so we
	 * don't risk running out of fds.
	 */
	HANDLE		hFile;
	int			ret;

	hFile = pgwin32_open_handle(name, O_RDONLY, true);
	if (hFile == INVALID_HANDLE_VALUE)
		return -1;

	ret = fileinfo_to_stat(hFile, buf);

	CloseHandle(hFile);
	return ret;
}

/*
 * Windows implementation of fstat().
 */
int
_pgfstat64(int fileno, struct stat *buf)
{
	HANDLE		hFile = (HANDLE) _get_osfhandle(fileno);
	DWORD		fileType = FILE_TYPE_UNKNOWN;
	unsigned short st_mode;

	if (buf == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	fileType = pgwin32_get_file_type(hFile);
	if (errno != 0)
		return -1;

	switch (fileType)
	{
			/* The specified file is a disk file */
		case FILE_TYPE_DISK:
			return fileinfo_to_stat(hFile, buf);

			/*
			 * The specified file is a socket, a named pipe, or an anonymous
			 * pipe.
			 */
		case FILE_TYPE_PIPE:
			st_mode = _S_IFIFO;
			break;
			/* The specified file is a character file */
		case FILE_TYPE_CHAR:
			st_mode = _S_IFCHR;
			break;
			/* Unused flag and unknown file type */
		case FILE_TYPE_REMOTE:
		case FILE_TYPE_UNKNOWN:
		default:
			errno = EINVAL;
			return -1;
	}

	memset(buf, 0, sizeof(*buf));
	buf->st_mode = st_mode;
	buf->st_dev = fileno;
	buf->st_rdev = fileno;
	buf->st_nlink = 1;
	return 0;
}

#endif							/* WIN32 */
