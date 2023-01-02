/*-------------------------------------------------------------------------
 *
 * open.c
 *	   Win32 open() replacement
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/port/open.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef WIN32

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "port/win32ntdll.h"

#include <fcntl.h>
#include <assert.h>
#include <sys/stat.h>

static int
openFlagsToCreateFileFlags(int openFlags)
{
	switch (openFlags & (O_CREAT | O_TRUNC | O_EXCL))
	{
			/* O_EXCL is meaningless without O_CREAT */
		case 0:
		case O_EXCL:
			return OPEN_EXISTING;

		case O_CREAT:
			return OPEN_ALWAYS;

			/* O_EXCL is meaningless without O_CREAT */
		case O_TRUNC:
		case O_TRUNC | O_EXCL:
			return TRUNCATE_EXISTING;

		case O_CREAT | O_TRUNC:
			return CREATE_ALWAYS;

			/* O_TRUNC is meaningless with O_CREAT */
		case O_CREAT | O_EXCL:
		case O_CREAT | O_TRUNC | O_EXCL:
			return CREATE_NEW;
	}

	/* will never get here */
	return 0;
}

/*
 * Internal function used by pgwin32_open() and _pgstat64().  When
 * backup_semantics is true, directories may be opened (for limited uses).  On
 * failure, INVALID_HANDLE_VALUE is returned and errno is set.
 */
HANDLE
pgwin32_open_handle(const char *fileName, int fileFlags, bool backup_semantics)
{
	HANDLE		h;
	SECURITY_ATTRIBUTES sa;
	int			loops = 0;

	if (initialize_ntdll() < 0)
		return INVALID_HANDLE_VALUE;

	/* Check that we can handle the request */
	assert((fileFlags & ((O_RDONLY | O_WRONLY | O_RDWR) | O_APPEND |
						 (O_RANDOM | O_SEQUENTIAL | O_TEMPORARY) |
						 _O_SHORT_LIVED | O_DSYNC | O_DIRECT |
						 (O_CREAT | O_TRUNC | O_EXCL) | (O_TEXT | O_BINARY))) == fileFlags);

	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	while ((h = CreateFile(fileName,
	/* cannot use O_RDONLY, as it == 0 */
						   (fileFlags & O_RDWR) ? (GENERIC_WRITE | GENERIC_READ) :
						   ((fileFlags & O_WRONLY) ? GENERIC_WRITE : GENERIC_READ),
	/* These flags allow concurrent rename/unlink */
						   (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
						   &sa,
						   openFlagsToCreateFileFlags(fileFlags),
						   FILE_ATTRIBUTE_NORMAL |
						   (backup_semantics ? FILE_FLAG_BACKUP_SEMANTICS : 0) |
						   ((fileFlags & O_RANDOM) ? FILE_FLAG_RANDOM_ACCESS : 0) |
						   ((fileFlags & O_SEQUENTIAL) ? FILE_FLAG_SEQUENTIAL_SCAN : 0) |
						   ((fileFlags & _O_SHORT_LIVED) ? FILE_ATTRIBUTE_TEMPORARY : 0) |
						   ((fileFlags & O_TEMPORARY) ? FILE_FLAG_DELETE_ON_CLOSE : 0) |
						   ((fileFlags & O_DIRECT) ? FILE_FLAG_NO_BUFFERING : 0) |
						   ((fileFlags & O_DSYNC) ? FILE_FLAG_WRITE_THROUGH : 0),
						   NULL)) == INVALID_HANDLE_VALUE)
	{
		/*
		 * Sharing violation or locking error can indicate antivirus, backup
		 * or similar software that's locking the file.  Wait a bit and try
		 * again, giving up after 30 seconds.
		 */
		DWORD		err = GetLastError();

		if (err == ERROR_SHARING_VIOLATION ||
			err == ERROR_LOCK_VIOLATION)
		{
#ifndef FRONTEND
			if (loops == 50)
				ereport(LOG,
						(errmsg("could not open file \"%s\": %s", fileName,
								(err == ERROR_SHARING_VIOLATION) ? _("sharing violation") : _("lock violation")),
						 errdetail("Continuing to retry for 30 seconds."),
						 errhint("You might have antivirus, backup, or similar software interfering with the database system.")));
#endif

			if (loops < 300)
			{
				pg_usleep(100000);
				loops++;
				continue;
			}
		}

		/*
		 * ERROR_ACCESS_DENIED is returned if the file is deleted but not yet
		 * gone (Windows NT status code is STATUS_DELETE_PENDING).  In that
		 * case, we'd better ask for the NT status too so we can translate it
		 * to a more Unix-like error.  We hope that nothing clobbers the NT
		 * status in between the internal NtCreateFile() call and CreateFile()
		 * returning.
		 *
		 * If there's no O_CREAT flag, then we'll pretend the file is
		 * invisible.  With O_CREAT, we have no choice but to report that
		 * there's a file in the way (which wouldn't happen on Unix).
		 */
		if (err == ERROR_ACCESS_DENIED &&
			pg_RtlGetLastNtStatus() == STATUS_DELETE_PENDING)
		{
			if (fileFlags & O_CREAT)
				err = ERROR_FILE_EXISTS;
			else
				err = ERROR_FILE_NOT_FOUND;
		}

		_dosmaperr(err);
		return INVALID_HANDLE_VALUE;
	}

	return h;
}

int
pgwin32_open(const char *fileName, int fileFlags,...)
{
	HANDLE		h;
	int			fd;

	h = pgwin32_open_handle(fileName, fileFlags, false);
	if (h == INVALID_HANDLE_VALUE)
		return -1;

#ifdef FRONTEND

	/*
	 * Since PostgreSQL 12, those concurrent-safe versions of open() and
	 * fopen() can be used by frontends, having as side-effect to switch the
	 * file-translation mode from O_TEXT to O_BINARY if none is specified.
	 * Caller may want to enforce the binary or text mode, but if nothing is
	 * defined make sure that the default mode maps with what versions older
	 * than 12 have been doing.
	 */
	if ((fileFlags & O_BINARY) == 0)
		fileFlags |= O_TEXT;
#endif

	/* _open_osfhandle will, on error, set errno accordingly */
	if ((fd = _open_osfhandle((intptr_t) h, fileFlags & O_APPEND)) < 0)
		CloseHandle(h);			/* will not affect errno */
	else if (fileFlags & (O_TEXT | O_BINARY) &&
			 _setmode(fd, fileFlags & (O_TEXT | O_BINARY)) < 0)
	{
		_close(fd);
		return -1;
	}

	return fd;
}

FILE *
pgwin32_fopen(const char *fileName, const char *mode)
{
	int			openmode = 0;
	int			fd;

	if (strstr(mode, "r+"))
		openmode |= O_RDWR;
	else if (strchr(mode, 'r'))
		openmode |= O_RDONLY;
	if (strstr(mode, "w+"))
		openmode |= O_RDWR | O_CREAT | O_TRUNC;
	else if (strchr(mode, 'w'))
		openmode |= O_WRONLY | O_CREAT | O_TRUNC;
	if (strchr(mode, 'a'))
		openmode |= O_WRONLY | O_CREAT | O_APPEND;

	if (strchr(mode, 'b'))
		openmode |= O_BINARY;
	if (strchr(mode, 't'))
		openmode |= O_TEXT;

	fd = pgwin32_open(fileName, openmode);
	if (fd == -1)
		return NULL;
	return _fdopen(fd, mode);
}

#endif
