/*-------------------------------------------------------------------------
 *
 * win32stat.c
 *	  Replacements for <sys/stat.h> functions using GetFileInformationByHandle
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/win32stat.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"
#include "port/win32ntdll.h"

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
 * Windows implementation of lstat().
 */
int
_pglstat64(const char *name, struct stat *buf)
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
	{
		if (errno == ENOENT)
		{
			/*
			 * If it's a junction point pointing to a non-existent path, we'll
			 * have ENOENT here (because pgwin32_open_handle does not use
			 * FILE_FLAG_OPEN_REPARSE_POINT).  In that case, we'll try again
			 * with readlink() below, which will distinguish true ENOENT from
			 * pseudo-symlink.
			 */
			memset(buf, 0, sizeof(*buf));
			ret = 0;
		}
		else
			return -1;
	}
	else
		ret = fileinfo_to_stat(hFile, buf);

	/*
	 * Junction points appear as directories to fileinfo_to_stat(), so we'll
	 * need to do a bit more work to distinguish them.
	 */
	if ((ret == 0 && S_ISDIR(buf->st_mode)) || hFile == INVALID_HANDLE_VALUE)
	{
		char		next[MAXPGPATH];
		ssize_t		size;

		/*
		 * POSIX says we need to put the length of the target path into
		 * st_size.  Use readlink() to get it, or learn that this is not a
		 * junction point.
		 */
		size = readlink(name, next, sizeof(next));
		if (size < 0)
		{
			if (errno == EACCES &&
				pg_RtlGetLastNtStatus() == STATUS_DELETE_PENDING)
			{
				/* Unlinked underneath us. */
				errno = ENOENT;
				ret = -1;
			}
			else if (errno == EINVAL)
			{
				/* It's not a junction point, nothing to do. */
			}
			else
			{
				/* Some other failure. */
				ret = -1;
			}
		}
		else
		{
			/* It's a junction point, so report it as a symlink. */
			buf->st_mode &= ~S_IFDIR;
			buf->st_mode |= S_IFLNK;
			buf->st_size = size;
			ret = 0;
		}
	}

	if (hFile != INVALID_HANDLE_VALUE)
		CloseHandle(hFile);
	return ret;
}

/*
 * Windows implementation of stat().
 */
int
_pgstat64(const char *name, struct stat *buf)
{
	int			loops = 0;
	int			ret;
	char		curr[MAXPGPATH];

	ret = _pglstat64(name, buf);

	strlcpy(curr, name, MAXPGPATH);

	/* Do we need to follow a symlink (junction point)? */
	while (ret == 0 && S_ISLNK(buf->st_mode))
	{
		char		next[MAXPGPATH];
		ssize_t		size;

		if (++loops > 8)
		{
			errno = ELOOP;
			return -1;
		}

		/*
		 * _pglstat64() already called readlink() once to be able to fill in
		 * st_size, and now we need to do it again to get the path to follow.
		 * That could be optimized, but stat() on symlinks is probably rare
		 * and this way is simple.
		 */
		size = readlink(curr, next, sizeof(next));
		if (size < 0)
		{
			if (errno == EACCES &&
				pg_RtlGetLastNtStatus() == STATUS_DELETE_PENDING)
			{
				/* Unlinked underneath us. */
				errno = ENOENT;
			}
			return -1;
		}
		if (size >= sizeof(next))
		{
			errno = ENAMETOOLONG;
			return -1;
		}
		next[size] = 0;

		ret = _pglstat64(next, buf);
		strcpy(curr, next);
	}

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
