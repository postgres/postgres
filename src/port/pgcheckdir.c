/*-------------------------------------------------------------------------
 *
 * src/port/pgcheckdir.c
 *
 * A simple subroutine to check whether a directory exists and is empty or not.
 * Useful in both initdb and the backend.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <dirent.h>


/*
 * Test to see if a directory exists and is empty or not.
 *
 * Returns:
 *		0 if nonexistent
 *		1 if exists and empty
 *		2 if exists and not empty
 *		-1 if trouble accessing directory (errno reflects the error)
 */
int
pg_check_dir(const char *dir)
{
	int			result = 1;
	DIR		   *chkdir;
	struct dirent *file;
	bool		dot_found = false;

	errno = 0;

	chkdir = opendir(dir);

	if (chkdir == NULL)
		return (errno == ENOENT) ? 0 : -1;

	while ((file = readdir(chkdir)) != NULL)
	{
		if (strcmp(".", file->d_name) == 0 ||
			strcmp("..", file->d_name) == 0)
		{
			/* skip this and parent directory */
			continue;
		}
#ifndef WIN32
		/* file starts with "." */
		else if (file->d_name[0] == '.')
		{
			dot_found = true;
		}
		else if (strcmp("lost+found", file->d_name) == 0)
		{
			result = 3;			/* not empty, mount point */
			break;
		}
#endif
		else
		{
			result = 4;			/* not empty */
			break;
		}
	}

#ifdef WIN32

	/*
	 * This fix is in mingw cvs (runtime/mingwex/dirent.c rev 1.4), but not in
	 * released version
	 */
	if (GetLastError() == ERROR_NO_MORE_FILES)
		errno = 0;
#endif

	closedir(chkdir);

	if (errno != 0)
		result = -1;			/* some kind of I/O error? */

	/* We report on dot-files if we _only_ find dot files */
	if (result == 1 && dot_found)
		result = 2;

	return result;
}
