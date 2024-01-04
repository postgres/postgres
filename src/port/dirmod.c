/*-------------------------------------------------------------------------
 *
 * dirmod.c
 *	  directory handling functions
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	This includes replacement versions of functions that work on
 *	Windows.
 *
 * IDENTIFICATION
 *	  src/port/dirmod.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

/* Don't modify declarations in system headers */
#if defined(WIN32) || defined(__CYGWIN__)
#undef rename
#undef unlink
#endif

#include <unistd.h>
#include <sys/stat.h>

#if defined(WIN32) || defined(__CYGWIN__)
#ifndef __CYGWIN__
#include <winioctl.h>
#else
#include <windows.h>
#include <w32api/winioctl.h>
#endif
#endif

#if defined(WIN32) && !defined(__CYGWIN__)
#include "port/win32ntdll.h"
#endif

#if defined(WIN32) || defined(__CYGWIN__)

/*
 *	pgrename
 */
int
pgrename(const char *from, const char *to)
{
	int			loops = 0;

	/*
	 * We need to loop because even though PostgreSQL uses flags that allow
	 * rename while the file is open, other applications might have the file
	 * open without those flags.  However, we won't wait indefinitely for
	 * someone else to close the file, as the caller might be holding locks
	 * and blocking other backends.
	 */
#if defined(WIN32) && !defined(__CYGWIN__)
	while (!MoveFileEx(from, to, MOVEFILE_REPLACE_EXISTING))
#else
	while (rename(from, to) < 0)
#endif
	{
#if defined(WIN32) && !defined(__CYGWIN__)
		DWORD		err = GetLastError();

		_dosmaperr(err);

		/*
		 * Modern NT-based Windows versions return ERROR_SHARING_VIOLATION if
		 * another process has the file open without FILE_SHARE_DELETE.
		 * ERROR_LOCK_VIOLATION has also been seen with some anti-virus
		 * software. This used to check for just ERROR_ACCESS_DENIED, so
		 * presumably you can get that too with some OS versions. We don't
		 * expect real permission errors where we currently use rename().
		 */
		if (err != ERROR_ACCESS_DENIED &&
			err != ERROR_SHARING_VIOLATION &&
			err != ERROR_LOCK_VIOLATION)
			return -1;
#else
		if (errno != EACCES)
			return -1;
#endif

		if (++loops > 100)		/* time out after 10 sec */
			return -1;
		pg_usleep(100000);		/* us */
	}
	return 0;
}

/*
 * Check if _pglstat64()'s reason for failure was STATUS_DELETE_PENDING.
 * This doesn't apply to Cygwin, which has its own lstat() that would report
 * the case as EACCES.
*/
static bool
lstat_error_was_status_delete_pending(void)
{
	if (errno != ENOENT)
		return false;
#if defined(WIN32) && !defined(__CYGWIN__)
	if (pg_RtlGetLastNtStatus() == STATUS_DELETE_PENDING)
		return true;
#endif
	return false;
}

/*
 *	pgunlink
 */
int
pgunlink(const char *path)
{
	bool		is_lnk;
	int			loops = 0;
	struct stat st;

	/*
	 * This function might be called for a regular file or for a junction
	 * point (which we use to emulate symlinks).  The latter must be unlinked
	 * with rmdir() on Windows.  Before we worry about any of that, let's see
	 * if we can unlink directly, since that's expected to be the most common
	 * case.
	 */
	if (unlink(path) == 0)
		return 0;
	if (errno != EACCES)
		return -1;

	/*
	 * EACCES is reported for many reasons including unlink() of a junction
	 * point.  Check if that's the case so we can redirect to rmdir().
	 *
	 * Note that by checking only once, we can't cope with a path that changes
	 * from regular file to junction point underneath us while we're retrying
	 * due to sharing violations, but that seems unlikely.  We could perhaps
	 * prevent that by holding a file handle ourselves across the lstat() and
	 * the retry loop, but that seems like over-engineering for now.
	 *
	 * In the special case of a STATUS_DELETE_PENDING error (file already
	 * unlinked, but someone still has it open), we don't want to report
	 * ENOENT to the caller immediately, because rmdir(parent) would probably
	 * fail. We want to wait until the file truly goes away so that simple
	 * recursive directory unlink algorithms work.
	 */
	if (lstat(path, &st) < 0)
	{
		if (lstat_error_was_status_delete_pending())
			is_lnk = false;
		else
			return -1;
	}
	else
		is_lnk = S_ISLNK(st.st_mode);

	/*
	 * We need to loop because even though PostgreSQL uses flags that allow
	 * unlink while the file is open, other applications might have the file
	 * open without those flags.  However, we won't wait indefinitely for
	 * someone else to close the file, as the caller might be holding locks
	 * and blocking other backends.
	 */
	while ((is_lnk ? rmdir(path) : unlink(path)) < 0)
	{
		if (errno != EACCES)
			return -1;
		if (++loops > 100)		/* time out after 10 sec */
			return -1;
		pg_usleep(100000);		/* us */
	}
	return 0;
}

/* We undefined these above; now redefine for possible use below */
#define rename(from, to)		pgrename(from, to)
#define unlink(path)			pgunlink(path)
#endif							/* defined(WIN32) || defined(__CYGWIN__) */


#if defined(WIN32) && !defined(__CYGWIN__)	/* Cygwin has its own symlinks */

/*
 *	pgsymlink support:
 *
 *	This struct is a replacement for REPARSE_DATA_BUFFER which is defined in VC6 winnt.h
 *	but omitted in later SDK functions.
 *	We only need the SymbolicLinkReparseBuffer part of the original struct's union.
 */
typedef struct
{
	DWORD		ReparseTag;
	WORD		ReparseDataLength;
	WORD		Reserved;
	/* SymbolicLinkReparseBuffer */
	WORD		SubstituteNameOffset;
	WORD		SubstituteNameLength;
	WORD		PrintNameOffset;
	WORD		PrintNameLength;
	WCHAR		PathBuffer[FLEXIBLE_ARRAY_MEMBER];
} REPARSE_JUNCTION_DATA_BUFFER;

#define REPARSE_JUNCTION_DATA_BUFFER_HEADER_SIZE   \
		FIELD_OFFSET(REPARSE_JUNCTION_DATA_BUFFER, SubstituteNameOffset)


/*
 *	pgsymlink - uses Win32 junction points
 *
 *	For reference:	http://www.codeproject.com/KB/winsdk/junctionpoints.aspx
 */
int
pgsymlink(const char *oldpath, const char *newpath)
{
	HANDLE		dirhandle;
	DWORD		len;
	char		buffer[MAX_PATH * sizeof(WCHAR) + offsetof(REPARSE_JUNCTION_DATA_BUFFER, PathBuffer)];
	char		nativeTarget[MAX_PATH];
	char	   *p = nativeTarget;
	REPARSE_JUNCTION_DATA_BUFFER *reparseBuf = (REPARSE_JUNCTION_DATA_BUFFER *) buffer;

	CreateDirectory(newpath, 0);
	dirhandle = CreateFile(newpath, GENERIC_READ | GENERIC_WRITE,
						   0, 0, OPEN_EXISTING,
						   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, 0);

	if (dirhandle == INVALID_HANDLE_VALUE)
	{
		_dosmaperr(GetLastError());
		return -1;
	}

	/* make sure we have an unparsed native win32 path */
	if (memcmp("\\??\\", oldpath, 4) != 0)
		snprintf(nativeTarget, sizeof(nativeTarget), "\\??\\%s", oldpath);
	else
		strlcpy(nativeTarget, oldpath, sizeof(nativeTarget));

	while ((p = strchr(p, '/')) != NULL)
		*p++ = '\\';

	len = strlen(nativeTarget) * sizeof(WCHAR);
	reparseBuf->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
	reparseBuf->ReparseDataLength = len + 12;
	reparseBuf->Reserved = 0;
	reparseBuf->SubstituteNameOffset = 0;
	reparseBuf->SubstituteNameLength = len;
	reparseBuf->PrintNameOffset = len + sizeof(WCHAR);
	reparseBuf->PrintNameLength = 0;
	MultiByteToWideChar(CP_ACP, 0, nativeTarget, -1,
						reparseBuf->PathBuffer, MAX_PATH);

	/*
	 * FSCTL_SET_REPARSE_POINT is coded differently depending on SDK version;
	 * we use our own definition
	 */
	if (!DeviceIoControl(dirhandle,
						 CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 41, METHOD_BUFFERED, FILE_ANY_ACCESS),
						 reparseBuf,
						 reparseBuf->ReparseDataLength + REPARSE_JUNCTION_DATA_BUFFER_HEADER_SIZE,
						 0, 0, &len, 0))
	{
		LPSTR		msg;
		int			save_errno;

		_dosmaperr(GetLastError());
		save_errno = errno;

		FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
					  FORMAT_MESSAGE_IGNORE_INSERTS |
					  FORMAT_MESSAGE_FROM_SYSTEM,
					  NULL, GetLastError(),
					  MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
					  (LPSTR) &msg, 0, NULL);
#ifndef FRONTEND
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not set junction for \"%s\": %s",
						nativeTarget, msg)));
#else
		fprintf(stderr, _("could not set junction for \"%s\": %s\n"),
				nativeTarget, msg);
#endif
		LocalFree(msg);

		CloseHandle(dirhandle);
		RemoveDirectory(newpath);

		errno = save_errno;

		return -1;
	}

	CloseHandle(dirhandle);

	return 0;
}

/*
 *	pgreadlink - uses Win32 junction points
 */
int
pgreadlink(const char *path, char *buf, size_t size)
{
	DWORD		attr;
	HANDLE		h;
	char		buffer[MAX_PATH * sizeof(WCHAR) + offsetof(REPARSE_JUNCTION_DATA_BUFFER, PathBuffer)];
	REPARSE_JUNCTION_DATA_BUFFER *reparseBuf = (REPARSE_JUNCTION_DATA_BUFFER *) buffer;
	DWORD		len;
	int			r;

	attr = GetFileAttributes(path);
	if (attr == INVALID_FILE_ATTRIBUTES)
	{
		_dosmaperr(GetLastError());
		return -1;
	}
	if ((attr & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
	{
		errno = EINVAL;
		return -1;
	}

	h = CreateFile(path,
				   GENERIC_READ,
				   FILE_SHARE_READ | FILE_SHARE_WRITE,
				   NULL,
				   OPEN_EXISTING,
				   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
				   0);
	if (h == INVALID_HANDLE_VALUE)
	{
		_dosmaperr(GetLastError());
		return -1;
	}

	if (!DeviceIoControl(h,
						 FSCTL_GET_REPARSE_POINT,
						 NULL,
						 0,
						 (LPVOID) reparseBuf,
						 sizeof(buffer),
						 &len,
						 NULL))
	{
		LPSTR		msg;

		errno = 0;
		FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
					  FORMAT_MESSAGE_IGNORE_INSERTS |
					  FORMAT_MESSAGE_FROM_SYSTEM,
					  NULL, GetLastError(),
					  MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
					  (LPSTR) &msg, 0, NULL);
#ifndef FRONTEND
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not get junction for \"%s\": %s",
						path, msg)));
#else
		fprintf(stderr, _("could not get junction for \"%s\": %s\n"),
				path, msg);
#endif
		LocalFree(msg);
		CloseHandle(h);
		errno = EINVAL;
		return -1;
	}
	CloseHandle(h);

	/* Got it, let's get some results from this */
	if (reparseBuf->ReparseTag != IO_REPARSE_TAG_MOUNT_POINT)
	{
		errno = EINVAL;
		return -1;
	}

	r = WideCharToMultiByte(CP_ACP, 0,
							reparseBuf->PathBuffer, -1,
							buf,
							size,
							NULL, NULL);

	if (r <= 0)
	{
		errno = EINVAL;
		return -1;
	}

	/* r includes the null terminator */
	r -= 1;

	/*
	 * If the path starts with "\??\" followed by a "drive absolute" path
	 * (known to Windows APIs as RtlPathTypeDriveAbsolute), then strip that
	 * prefix.  This undoes some of the transformation performed by
	 * pgsymlink(), to get back to a format that users are used to seeing.  We
	 * don't know how to transform other path types that might be encountered
	 * outside PGDATA, so we just return them directly.
	 */
	if (r >= 7 &&
		buf[0] == '\\' &&
		buf[1] == '?' &&
		buf[2] == '?' &&
		buf[3] == '\\' &&
		isalpha(buf[4]) &&
		buf[5] == ':' &&
		buf[6] == '\\')
	{
		memmove(buf, buf + 4, strlen(buf + 4) + 1);
		r -= 4;
	}
	return r;
}

#endif							/* defined(WIN32) && !defined(__CYGWIN__) */
