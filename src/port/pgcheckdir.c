/*-------------------------------------------------------------------------
 *
 * src/port/pgcheckdir.c
 *
 * A simple subroutine to check whether a directory exists and is empty or not.
 * Useful in both initdb and the backend.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
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

	chkdir = opendir(dir);

	if (chkdir == NULL)
		return (errno == ENOENT) ? 0 : -1;

	while (errno = 0, (file = readdir(chkdir)) != NULL)
	{
		if (strcmp(".", file->d_name) == 0 ||
			strcmp("..", file->d_name) == 0)
		{
			/* skip this and parent directory */
			continue;
		}
		else
		{
			result = 2;			/* not empty */
			break;
		}
	}

#ifdef WIN32
	/* Bug in old Mingw dirent.c;  fixed in mingw-runtime-3.2, 2003-10-10 */
	if (GetLastError() == ERROR_NO_MORE_FILES)
		errno = 0;
#endif

	if (errno || closedir(chkdir))
		result = -1;			/* some kind of I/O error? */

	return result;
}
