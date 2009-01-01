/*-------------------------------------------------------------------------
 *
 * copydir.c
 *	  copies a directory
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	While "xcopy /e /i /q" works fine for copying directories, on Windows XP
 *	it requires a Window handle which prevents it from working when invoked
 *	as a service.
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/copydir.c,v 1.23 2009/01/01 17:24:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "storage/fd.h"

/*
 *	On Windows, call non-macro versions of palloc; we can't reference
 *	CurrentMemoryContext in this file because of PGDLLIMPORT conflict.
 */
#if defined(WIN32) || defined(__CYGWIN__)
#undef palloc
#undef pstrdup
#define palloc(sz)		pgport_palloc(sz)
#define pstrdup(str)	pgport_pstrdup(str)
#endif


static void copy_file(char *fromfile, char *tofile);


/*
 * copydir: copy a directory
 *
 * If recurse is false, subdirectories are ignored.  Anything that's not
 * a directory or a regular file is ignored.
 */
void
copydir(char *fromdir, char *todir, bool recurse)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		fromfile[MAXPGPATH];
	char		tofile[MAXPGPATH];

	if (mkdir(todir, S_IRUSR | S_IWUSR | S_IXUSR) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", todir)));

	xldir = AllocateDir(fromdir);
	if (xldir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", fromdir)));

	while ((xlde = ReadDir(xldir, fromdir)) != NULL)
	{
		struct stat fst;

		if (strcmp(xlde->d_name, ".") == 0 ||
			strcmp(xlde->d_name, "..") == 0)
			continue;

		snprintf(fromfile, MAXPGPATH, "%s/%s", fromdir, xlde->d_name);
		snprintf(tofile, MAXPGPATH, "%s/%s", todir, xlde->d_name);

		if (lstat(fromfile, &fst) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", fromfile)));

		if (S_ISDIR(fst.st_mode))
		{
			/* recurse to handle subdirectories */
			if (recurse)
				copydir(fromfile, tofile, true);
		}
		else if (S_ISREG(fst.st_mode))
			copy_file(fromfile, tofile);
	}

	FreeDir(xldir);
}

/*
 * copy one file
 */
static void
copy_file(char *fromfile, char *tofile)
{
	char	   *buffer;
	int			srcfd;
	int			dstfd;
	int			nbytes;

	/* Use palloc to ensure we get a maxaligned buffer */
#define COPY_BUF_SIZE (8 * BLCKSZ)

	buffer = palloc(COPY_BUF_SIZE);

	/*
	 * Open the files
	 */
	srcfd = BasicOpenFile(fromfile, O_RDONLY | PG_BINARY, 0);
	if (srcfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fromfile)));

	dstfd = BasicOpenFile(tofile, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
						  S_IRUSR | S_IWUSR);
	if (dstfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tofile)));

	/*
	 * Do the data copying.
	 */
	for (;;)
	{
		nbytes = read(srcfd, buffer, COPY_BUF_SIZE);
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", fromfile)));
		if (nbytes == 0)
			break;
		errno = 0;
		if ((int) write(dstfd, buffer, nbytes) != nbytes)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", tofile)));
		}
	}

	/*
	 * Be paranoid here to ensure we catch problems.
	 */
	if (pg_fsync(dstfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tofile)));

	if (close(dstfd))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tofile)));

	close(srcfd);

	pfree(buffer);
}
