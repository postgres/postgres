/*-------------------------------------------------------------------------
 *
 * open.c
 *	   Win32 open() replacement
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
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
 *	 - file attribute setting, based on fileMode?
 */
int
pgwin32_open(const char *fileName, int fileFlags,...)
{
	int			fd;
	HANDLE		h = INVALID_HANDLE_VALUE;
	SECURITY_ATTRIBUTES sa;
	int			loops = 0;

	/* Check that we can handle the request */
	assert((fileFlags & ((O_RDONLY | O_WRONLY | O_RDWR) | O_APPEND |
						 (O_RANDOM | O_SEQUENTIAL | O_TEMPORARY) |
						 _O_SHORT_LIVED | O_DSYNC | O_DIRECT |
						 (O_CREAT | O_TRUNC | O_EXCL) | (O_TEXT | O_BINARY))) == fileFlags);
#ifndef FRONTEND
	Assert(pgwin32_signal_event != NULL);	/* small chance of pg_usleep() */
#endif

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
				struct stat st;

				if (stat(fileName, &st) != 0)
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
