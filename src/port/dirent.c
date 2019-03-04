/*-------------------------------------------------------------------------
 *
 * dirent.c
 *	  opendir/readdir/closedir for win32/msvc
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/dirent.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <dirent.h>


struct DIR
{
	char	   *dirname;
	struct dirent ret;			/* Used to return to caller */
	HANDLE		handle;
};

DIR *
opendir(const char *dirname)
{
	DWORD		attr;
	DIR		   *d;

	/* Make sure it is a directory */
	attr = GetFileAttributes(dirname);
	if (attr == INVALID_FILE_ATTRIBUTES)
	{
		errno = ENOENT;
		return NULL;
	}
	if ((attr & FILE_ATTRIBUTE_DIRECTORY) != FILE_ATTRIBUTE_DIRECTORY)
	{
		errno = ENOTDIR;
		return NULL;
	}

	d = malloc(sizeof(DIR));
	if (!d)
	{
		errno = ENOMEM;
		return NULL;
	}
	d->dirname = malloc(strlen(dirname) + 4);
	if (!d->dirname)
	{
		errno = ENOMEM;
		free(d);
		return NULL;
	}
	strcpy(d->dirname, dirname);
	if (d->dirname[strlen(d->dirname) - 1] != '/' &&
		d->dirname[strlen(d->dirname) - 1] != '\\')
		strcat(d->dirname, "\\");		/* Append backslash if not already
										 * there */
	strcat(d->dirname, "*");	/* Search for entries named anything */
	d->handle = INVALID_HANDLE_VALUE;
	d->ret.d_ino = 0;			/* no inodes on win32 */
	d->ret.d_reclen = 0;		/* not used on win32 */

	return d;
}

struct dirent *
readdir(DIR *d)
{
	WIN32_FIND_DATA fd;

	if (d->handle == INVALID_HANDLE_VALUE)
	{
		d->handle = FindFirstFile(d->dirname, &fd);
		if (d->handle == INVALID_HANDLE_VALUE)
		{
			/* If there are no files, force errno=0 (unlike mingw) */
			if (GetLastError() == ERROR_FILE_NOT_FOUND)
				errno = 0;
			else
				_dosmaperr(GetLastError());
			return NULL;
		}
	}
	else
	{
		if (!FindNextFile(d->handle, &fd))
		{
			/* If there are no more files, force errno=0 (like mingw) */
			if (GetLastError() == ERROR_NO_MORE_FILES)
				errno = 0;
			else
				_dosmaperr(GetLastError());
			return NULL;
		}
	}
	strcpy(d->ret.d_name, fd.cFileName);		/* Both strings are MAX_PATH
												 * long */
	d->ret.d_namlen = strlen(d->ret.d_name);

	return &d->ret;
}

int
closedir(DIR *d)
{
	int			ret = 0;

	if (d->handle != INVALID_HANDLE_VALUE)
		ret = !FindClose(d->handle);
	free(d->dirname);
	free(d);

	return ret;
}
