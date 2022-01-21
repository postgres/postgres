/*-------------------------------------------------------------------------
 *
 * rmtree.c
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/rmtree.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <unistd.h>
#include <sys/stat.h>

#ifndef FRONTEND
#define pg_log_warning(...) elog(WARNING, __VA_ARGS__)
#else
#include "common/logging.h"
#endif


/*
 *	rmtree
 *
 *	Delete a directory tree recursively.
 *	Assumes path points to a valid directory.
 *	Deletes everything under path.
 *	If rmtopdir is true deletes the directory too.
 *	Returns true if successful, false if there was any problem.
 *	(The details of the problem are reported already, so caller
 *	doesn't really have to say anything more, but most do.)
 */
bool
rmtree(const char *path, bool rmtopdir)
{
	bool		result = true;
	char		pathbuf[MAXPGPATH];
	char	  **filenames;
	char	  **filename;
	struct stat statbuf;

	/*
	 * we copy all the names out of the directory before we start modifying
	 * it.
	 */
	filenames = pgfnames(path);

	if (filenames == NULL)
		return false;

	/* now we have the names we can start removing things */
	for (filename = filenames; *filename; filename++)
	{
		snprintf(pathbuf, MAXPGPATH, "%s/%s", path, *filename);

		/*
		 * It's ok if the file is not there anymore; we were just about to
		 * delete it anyway.
		 *
		 * This is not an academic possibility. One scenario where this
		 * happens is when bgwriter has a pending unlink request for a file in
		 * a database that's being dropped. In dropdb(), we call
		 * ForgetDatabaseSyncRequests() to flush out any such pending unlink
		 * requests, but because that's asynchronous, it's not guaranteed that
		 * the bgwriter receives the message in time.
		 */
		if (lstat(pathbuf, &statbuf) != 0)
		{
			if (errno != ENOENT)
			{
				pg_log_warning("could not stat file or directory \"%s\": %m",
							   pathbuf);
				result = false;
			}
			continue;
		}

		if (S_ISDIR(statbuf.st_mode))
		{
			/* call ourselves recursively for a directory */
			if (!rmtree(pathbuf, true))
			{
				/* we already reported the error */
				result = false;
			}
		}
		else
		{
			if (unlink(pathbuf) != 0)
			{
				if (errno != ENOENT)
				{
					pg_log_warning("could not remove file or directory \"%s\": %m",
								   pathbuf);
					result = false;
				}
			}
		}
	}

	if (rmtopdir)
	{
		if (rmdir(path) != 0)
		{
			pg_log_warning("could not remove file or directory \"%s\": %m",
						   path);
			result = false;
		}
	}

	pgfnames_cleanup(filenames);

	return result;
}
