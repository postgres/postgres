/*-------------------------------------------------------------------------
 *
 * version.c
 *		Routine to retrieve information of PG_VERSION
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/fe_utils/version.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/stat.h>

#include "common/logging.h"
#include "fe_utils/version.h"

/*
 * Assumed maximum size of PG_VERSION.  This should be more than enough for
 * any version numbers that need to be handled.
 */
#define PG_VERSION_MAX_SIZE	64

/*
 * get_pg_version
 *
 * Retrieve the major version number of the given data folder, from
 * PG_VERSION.  The result returned is a version number, that can be used
 * for comparisons based on PG_VERSION_NUM.  For example, if PG_VERSION
 * contains "18\n", this function returns 180000.
 *
 * This supports both the pre-v10 and the post-v10 version numbering.
 *
 * Optionally, "version_str" can be specified to store the contents
 * retrieved from PG_VERSION.  It is allocated by this routine; the
 * caller is responsible for pg_free()-ing it.
 */
uint32
get_pg_version(const char *datadir, char **version_str)
{
	FILE	   *version_fd;
	char		ver_filename[MAXPGPATH];
	char		buf[PG_VERSION_MAX_SIZE];
	int			v1 = 0,
				v2 = 0;
	struct stat st;

	snprintf(ver_filename, sizeof(ver_filename), "%s/PG_VERSION",
			 datadir);

	if ((version_fd = fopen(ver_filename, "r")) == NULL)
		pg_fatal("could not open version file \"%s\": %m", ver_filename);

	if (fstat(fileno(version_fd), &st) != 0)
		pg_fatal("could not stat file \"%s\": %m", ver_filename);
	if (st.st_size > PG_VERSION_MAX_SIZE)
		pg_fatal("file \"%s\" is too large", ver_filename);

	if (fscanf(version_fd, "%63s", buf) == 0 ||
		sscanf(buf, "%d.%d", &v1, &v2) < 1)
		pg_fatal("could not parse version file \"%s\"", ver_filename);

	fclose(version_fd);

	if (version_str)
	{
		*version_str = pg_malloc(PG_VERSION_MAX_SIZE);
		memcpy(*version_str, buf, st.st_size);
	}

	if (v1 < 10)
	{
		/* pre-v10 style, e.g. 9.6.1 */
		return v1 * 10000 + v2 * 100;
	}
	else
	{
		/* post-v10 style, e.g. 10.1 */
		return v1 * 10000;
	}
}
