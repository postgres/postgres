/*-------------------------------------------------------------------------
 *
 * copydir.c
 *	  copies a directory
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	While "xcopy /e /i /q" works fine for copying directories, on Windows XP
 *	it requires a Window handle which prevents it from working when invoked
 *	as a service.
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/copydir.c,v 1.9 2004/08/29 04:13:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/fd.h"

#undef mkdir					/* no reason to use that macro because we
								 * ignore the 2nd arg */


/*
 * copydir: copy a directory (we only need to go one level deep)
 *
 * Return 0 on success, nonzero on failure.
 *
 * NB: do not elog(ERROR) on failure.  Return to caller so it can try to
 * clean up.
 */
int
copydir(char *fromdir, char *todir)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		fromfl[MAXPGPATH];
	char		tofl[MAXPGPATH];

	if (mkdir(todir) != 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", todir)));
		return -1;
	}
	xldir = AllocateDir(fromdir);
	if (xldir == NULL)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", fromdir)));
		return -1;
	}

	while ((xlde = readdir(xldir)) != NULL)
	{
		snprintf(fromfl, MAXPGPATH, "%s/%s", fromdir, xlde->d_name);
		snprintf(tofl, MAXPGPATH, "%s/%s", todir, xlde->d_name);
		if (CopyFile(fromfl, tofl, TRUE) < 0)
		{
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not copy file \"%s\": %m", fromfl)));
			FreeDir(xldir);
			return -1;
		}
	}

	FreeDir(xldir);
	return 0;
}
