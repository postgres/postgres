/*-------------------------------------------------------------------------
 *
 * version.c
 *	  Routines to handle Postgres version number.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/utils/Attic/version.c,v 1.14 2000/01/26 05:58:53 momjian Exp $
 *
 *	STANDALONE CODE - do not use error routines as this code is not linked
 *	with any...
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <sys/file.h>
#include <fcntl.h>				/* For open() flags */
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#include "postgres.h"

#include "storage/fd.h"			/* for O_ */

#include "version.h"


static void
PathSetVersionFilePath(const char *path, char *filepathbuf)
{
/*----------------------------------------------------------------------------
  PathSetVersionFilePath

  Destructively change "filepathbuf" to contain the concatenation of "path"
  and the name of the version file name.
----------------------------------------------------------------------------*/
	if ((strlen(path) + 1 + strlen(PG_VERFILE)) >= MAXPGPATH)
		*filepathbuf = '\0';
	else
		sprintf(filepathbuf, "%s%c%s", path, SEP_CHAR, PG_VERFILE);
}



void
ValidatePgVersion(const char *path, char **reason_p)
{
/*----------------------------------------------------------------------------
	Determine whether the PG_VERSION file in directory <path> indicates
	a data version compatible with the version of this program.

	If compatible, return <*reason_p> == NULL.	Otherwise, malloc space,
	fill it with a text string explaining how it isn't compatible (or why
	we can't tell), and return a pointer to that space as <*reason_p>.
-----------------------------------------------------------------------------*/
	int			fd;
	int			nread;
	char		myversion[32];
	char		version[32];
	char		full_path[MAXPGPATH];

	PathSetVersionFilePath(path, full_path);

	sprintf(myversion, "%s.%s\n", PG_RELEASE, PG_VERSION);

#ifndef __CYGWIN32__
	if ((fd = open(full_path, O_RDONLY, 0)) == -1)
#else
	if ((fd = open(full_path, O_RDONLY | O_BINARY, 0)) == -1)
#endif
	{
		*reason_p = malloc(100 + strlen(full_path));
		sprintf(*reason_p, "File '%s' does not exist or no read permission.", full_path);
	}
	else
	{
		nread = read(fd, version, sizeof(version)-1);
		if (nread < 4 ||
			!isdigit(version[0]) ||
			version[nread-1] != '\n')
		{
			*reason_p = malloc(100 + strlen(full_path));
			sprintf(*reason_p, "File '%s' does not have a valid format "
					"for a PG_VERSION file.", full_path);
		}
		else
		{
			version[nread] = '\0';
			if (strcmp(version, myversion) != 0)
			{
				*reason_p = malloc(200 + strlen(full_path));
				sprintf(*reason_p,
						"Version number in file '%s' should be %s, "
						"not %s.",
						full_path, myversion, version);
			}
			else
				*reason_p = NULL;
		}
		close(fd);
	}
}



void
SetPgVersion(const char *path, char **reason_p)
{
/*---------------------------------------------------------------------------
  Create the PG_VERSION file in the directory <path>.

  If we fail, allocate storage, fill it with a text string explaining why,
  and return a pointer to that storage as <*reason_p>.	If we succeed,
  return *reason_p = NULL.
---------------------------------------------------------------------------*/
	int			fd;
	char		version[32];
	char		full_path[MAXPGPATH];

	PathSetVersionFilePath(path, full_path);

	sprintf(version, "%s.%s\n", PG_RELEASE, PG_VERSION);

#ifndef __CYGWIN32__
	fd = open(full_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
#else
	fd = open(full_path, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0666);
#endif
	if (fd < 0)
	{
		*reason_p = malloc(100 + strlen(full_path));
		sprintf(*reason_p,
				"Unable to create file '%s', errno from open(): %s (%d).",
				full_path, strerror(errno), errno);
	}
	else
	{
		int			rc;			/* return code from some function we call */

		rc = write(fd, version, strlen(version));
		if (rc != strlen(version))
		{
			*reason_p = malloc(100 + strlen(full_path));
			sprintf(*reason_p,
					"Failed to write to file '%s', after it was already "
					"open.  Errno from write(): %s (%d)",
					full_path, strerror(errno), errno);
		}
		else
			*reason_p = NULL;
		close(fd);
	}
}
