/*-------------------------------------------------------------------------
 *
 * win32stat.c
 *	  Replacements for <sys/stat.h> functions using GetFileInformationByHandle
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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

	uxmode |= (unsigned short) (attr & FILE_ATTRIBUTE_READONLY) ?
		(_S_IREAD) : (_S_IREAD | _S_IWRITE);

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

	buf->st_size = (((uint64) fiData.nFileSizeHigh) << 32) |
		(uint64) fiData.nFileSizeLow;

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
	 * We must use a handle so lstat() returns the information of the target
	 * file.  To have a reliable test for ERROR_DELETE_PENDING, this uses a
	 * method similar to open() with a loop using stat() and some waits when
	 * facing ERROR_ACCESS_DENIED.
	 */
	SECURITY_ATTRIBUTES sa;
	HANDLE		hFile;
	int			ret;
	int			loops = 0;

	if (name == NULL || buf == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	/* fast not-exists check */
	if (GetFileAttributes(name) == INVALID_FILE_ATTRIBUTES)
	{
		DWORD		err = GetLastError();

		if (err != ERROR_ACCESS_DENIED)
		{
			_dosmaperr(err);
			return -1;
		}
	}

	/* get a file handle as lightweight as we can */
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;
	while ((hFile = CreateFile(name,
							   GENERIC_READ,
							   (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
							   &sa,
							   OPEN_EXISTING,
							   (FILE_FLAG_NO_BUFFERING | FILE_FLAG_BACKUP_SEMANTICS |
								FILE_FLAG_OVERLAPPED),
							   NULL)) == INVALID_HANDLE_VALUE)
	{
		DWORD		err = GetLastError();

		/*
		 * ERROR_ACCESS_DENIED is returned if the file is deleted but not yet
		 * gone (Windows NT status code is STATUS_DELETE_PENDING).  In that
		 * case we want to wait a bit and try again, giving up after 1 second
		 * (since this condition should never persist very long).  However,
		 * there are other commonly-hit cases that return ERROR_ACCESS_DENIED,
		 * so care is needed.  In particular that happens if we try to open a
		 * directory, or of course if there's an actual file-permissions
		 * problem.  To distinguish these cases, try a stat().  In the
		 * delete-pending case, it will either also get STATUS_DELETE_PENDING,
		 * or it will see the file as gone and fail with ENOENT.  In other
		 * cases it will usually succeed.  The only somewhat-likely case where
		 * this coding will uselessly wait is if there's a permissions problem
		 * with a containing directory, which we hope will never happen in any
		 * performance-critical code paths.
		 */
		if (err == ERROR_ACCESS_DENIED)
		{
			if (loops < 10)
			{
				struct microsoft_native_stat st;

				if (microsoft_native_stat(name, &st) != 0)
				{
					pg_usleep(100000);
					loops++;
					continue;
				}
			}
		}

		_dosmaperr(err);
		return -1;
	}

	/* At last we can invoke fileinfo_to_stat */
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

	if (hFile == INVALID_HANDLE_VALUE || buf == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	/*
	 * Since we already have a file handle there is no need to check for
	 * ERROR_DELETE_PENDING.
	 */

	return fileinfo_to_stat(hFile, buf);
}

#endif							/* WIN32 */
