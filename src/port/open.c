/*-------------------------------------------------------------------------
 *
 * open.c
 *	   Win32 open() replacement
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/port/open.c,v 1.11 2005/10/15 02:49:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifdef WIN32

#include "c.h"

#include <windows.h>
#include <fcntl.h>
#include <assert.h>

int			win32_open(const char *fileName, int fileFlags,...);

static int
openFlagsToCreateFileFlags(int openFlags)
{
	switch (openFlags & (O_CREAT | O_TRUNC | O_EXCL))
	{
		case 0:
		case O_EXCL:
			return OPEN_EXISTING;

		case O_CREAT:
			return OPEN_ALWAYS;

		case O_TRUNC:
		case O_TRUNC | O_EXCL:
			return TRUNCATE_EXISTING;

		case O_CREAT | O_TRUNC:
			return CREATE_ALWAYS;

		case O_CREAT | O_EXCL:
		case O_CREAT | O_TRUNC | O_EXCL:
			return CREATE_NEW;
	}

	/* will never get here */
	return 0;
}

/*
 *	 - file attribute setting, based on fileMode?
 *	 - handle other flags? (eg FILE_FLAG_NO_BUFFERING/FILE_FLAG_WRITE_THROUGH)
 */
int
win32_open(const char *fileName, int fileFlags,...)
{
	int			fd;
	HANDLE		h;
	SECURITY_ATTRIBUTES sa;

	/* Check that we can handle the request */
	assert((fileFlags & ((O_RDONLY | O_WRONLY | O_RDWR) | O_APPEND |
						 (O_RANDOM | O_SEQUENTIAL | O_TEMPORARY) |
						 _O_SHORT_LIVED | O_DSYNC |
		  (O_CREAT | O_TRUNC | O_EXCL) | (O_TEXT | O_BINARY))) == fileFlags);

	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	if ((h = CreateFile(fileName,
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
						((fileFlags & O_DSYNC) ? FILE_FLAG_WRITE_THROUGH : 0),
						NULL)) == INVALID_HANDLE_VALUE)
	{
		switch (GetLastError())
		{
				/* EMFILE, ENFILE should not occur from CreateFile. */
			case ERROR_PATH_NOT_FOUND:
			case ERROR_FILE_NOT_FOUND:
				errno = ENOENT;
				break;
			case ERROR_FILE_EXISTS:
				errno = EEXIST;
				break;
			case ERROR_ACCESS_DENIED:
				errno = EACCES;
				break;
			default:
				errno = EINVAL;
		}
		return -1;
	}

	/* _open_osfhandle will, on error, set errno accordingly */
	if ((fd = _open_osfhandle((long) h, fileFlags & O_APPEND)) < 0 ||
		(fileFlags & (O_TEXT | O_BINARY) && (_setmode(fd, fileFlags & (O_TEXT | O_BINARY)) < 0)))
		CloseHandle(h);			/* will not affect errno */
	return fd;
}

#endif
