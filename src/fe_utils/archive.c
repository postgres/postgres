/*-------------------------------------------------------------------------
 *
 * archive.c
 *	  Routines to access WAL archives from frontend
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/fe_utils/archive.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/xlog_internal.h"
#include "common/archive.h"
#include "common/logging.h"
#include "fe_utils/archive.h"


/*
 * RestoreArchivedFile
 *
 * Attempt to retrieve the specified file from off-line archival storage.
 * If successful, return a file descriptor of the restored file, else
 * return -1.
 *
 * For fixed-size files, the caller may pass the expected size as an
 * additional crosscheck on successful recovery.  If the file size is not
 * known, set expectedSize = 0.
 */
int
RestoreArchivedFile(const char *path, const char *xlogfname,
					off_t expectedSize, const char *restoreCommand)
{
	char		xlogpath[MAXPGPATH];
	char	   *xlogRestoreCmd;
	int			rc;
	struct stat stat_buf;

	snprintf(xlogpath, MAXPGPATH, "%s/" XLOGDIR "/%s", path, xlogfname);

	xlogRestoreCmd = BuildRestoreCommand(restoreCommand, xlogpath,
										 xlogfname, NULL);

	/*
	 * Execute restore_command, which should copy the missing file from
	 * archival storage.
	 */
	fflush(NULL);
	rc = system(xlogRestoreCmd);
	pfree(xlogRestoreCmd);

	if (rc == 0)
	{
		/*
		 * Command apparently succeeded, but let's make sure the file is
		 * really there now and has the correct size.
		 */
		if (stat(xlogpath, &stat_buf) == 0)
		{
			if (expectedSize > 0 && stat_buf.st_size != expectedSize)
				pg_fatal("unexpected file size for \"%s\": %lld instead of %lld",
						 xlogfname, (long long int) stat_buf.st_size,
						 (long long int) expectedSize);
			else
			{
				int			xlogfd = open(xlogpath, O_RDONLY | PG_BINARY, 0);

				if (xlogfd < 0)
					pg_fatal("could not open file \"%s\" restored from archive: %m",
							 xlogpath);
				else
					return xlogfd;
			}
		}
		else
		{
			if (errno != ENOENT)
				pg_fatal("could not stat file \"%s\": %m",
						 xlogpath);
		}
	}

	/*
	 * If the failure was due to a signal, then it would be misleading to
	 * return with a failure at restoring the file.  So just bail out and
	 * exit.  Hard shell errors such as "command not found" are treated as
	 * fatal too.
	 */
	if (wait_result_is_any_signal(rc, true))
		pg_fatal("\"restore_command\" failed: %s",
				 wait_result_to_str(rc));

	/*
	 * The file is not available, so just let the caller decide what to do
	 * next.
	 */
	pg_log_error("could not restore file \"%s\" from archive",
				 xlogfname);
	return -1;
}
