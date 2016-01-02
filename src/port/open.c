/*-------------------------------------------------------------------------
 *
 * open.c
 *	   Win32 open() replacement
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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

#include <windows.h>
#include <fcntl.h>
#include <assert.h>


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
		 * or similar software that's locking the file. Try again for 30
		 * seconds before giving up.
		 */
		DWORD		err = GetLastError();

		if (err == ERROR_SHARING_VIOLATION ||
			err == ERROR_LOCK_VIOLATION)
		{
			pg_usleep(100000);
			loops++;

#ifndef FRONTEND
			if (loops == 50)
				ereport(LOG,
						(errmsg("could not open file \"%s\": %s", fileName,
								(err == ERROR_SHARING_VIOLATION) ? _("sharing violation") : _("lock violation")),
						 errdetail("Continuing to retry for 30 seconds."),
						 errhint("You might have antivirus, backup, or similar software interfering with the database system.")));
#endif

			if (loops < 300)
				continue;
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
