/*-------------------------------------------------------------------------
 *
 * rmtree.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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

#include "common/file_utils.h"

#ifndef FRONTEND
#include "storage/fd.h"
#define pg_log_warning(...) elog(WARNING, __VA_ARGS__)
#define LOG_LEVEL WARNING
#define OPENDIR(x) AllocateDir(x)
#define CLOSEDIR(x) FreeDir(x)
#else
#include "common/logging.h"
#define LOG_LEVEL PG_LOG_WARNING
#define OPENDIR(x) opendir(x)
#define CLOSEDIR(x) closedir(x)
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
	char		pathbuf[MAXPGPATH];
	DIR		   *dir;
	struct dirent *de;
	bool		result = true;
	size_t		dirnames_size = 0;
	size_t		dirnames_capacity = 8;
	char	  **dirnames;

	dir = OPENDIR(path);
	if (dir == NULL)
	{
		pg_log_warning("could not open directory \"%s\": %m", path);
		return false;
	}

	dirnames = (char **) palloc(sizeof(char *) * dirnames_capacity);

	while (errno = 0, (de = readdir(dir)))
	{
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path, de->d_name);
		switch (get_dirent_type(pathbuf, de, false, LOG_LEVEL))
		{
			case PGFILETYPE_ERROR:
				/* already logged, press on */
				break;
			case PGFILETYPE_DIR:

				/*
				 * Defer recursion until after we've closed this directory, to
				 * avoid using more than one file descriptor at a time.
				 */
				if (dirnames_size == dirnames_capacity)
				{
					dirnames = repalloc(dirnames,
										sizeof(char *) * dirnames_capacity * 2);
					dirnames_capacity *= 2;
				}
				dirnames[dirnames_size++] = pstrdup(pathbuf);
				break;
			default:
				if (unlink(pathbuf) != 0 && errno != ENOENT)
				{
					pg_log_warning("could not remove file \"%s\": %m", pathbuf);
					result = false;
				}
				break;
		}
	}

	if (errno != 0)
	{
		pg_log_warning("could not read directory \"%s\": %m", path);
		result = false;
	}

	CLOSEDIR(dir);

	/* Now recurse into the subdirectories we found. */
	for (size_t i = 0; i < dirnames_size; ++i)
	{
		if (!rmtree(dirnames[i], true))
			result = false;
		pfree(dirnames[i]);
	}

	if (rmtopdir)
	{
		if (rmdir(path) != 0)
		{
			pg_log_warning("could not remove directory \"%s\": %m", path);
			result = false;
		}
	}

	pfree(dirnames);

	return result;
}
