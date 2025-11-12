/*-------------------------------------------------------------------------
 *
 * pg_tablespace.c
 *	  routines to support manipulation of the pg_tablespace relation
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_tablespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "miscadmin.h"


/*
 * get_tablespace_location
 *		Get a tablespace's location as a C-string, by its OID
 */
char *
get_tablespace_location(Oid tablespaceOid)
{
	char		sourcepath[MAXPGPATH];
	char		targetpath[MAXPGPATH];
	int			rllen;
	struct stat st;

	/*
	 * It's useful to apply this to pg_class.reltablespace, wherein zero means
	 * "the database's default tablespace".  So, rather than throwing an error
	 * for zero, we choose to assume that's what is meant.
	 */
	if (tablespaceOid == InvalidOid)
		tablespaceOid = MyDatabaseTableSpace;

	/*
	 * Return empty string for the cluster's default tablespaces
	 */
	if (tablespaceOid == DEFAULTTABLESPACE_OID ||
		tablespaceOid == GLOBALTABLESPACE_OID)
		return pstrdup("");

	/*
	 * Find the location of the tablespace by reading the symbolic link that
	 * is in pg_tblspc/<oid>.
	 */
	snprintf(sourcepath, sizeof(sourcepath), "%s/%u", PG_TBLSPC_DIR, tablespaceOid);

	/*
	 * Before reading the link, check if the source path is a link or a
	 * junction point.  Note that a directory is possible for a tablespace
	 * created with allow_in_place_tablespaces enabled.  If a directory is
	 * found, a relative path to the data directory is returned.
	 */
	if (lstat(sourcepath, &st) < 0)
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not stat file \"%s\": %m",
					   sourcepath));

	if (!S_ISLNK(st.st_mode))
		return pstrdup(sourcepath);

	/*
	 * In presence of a link or a junction point, return the path pointed to.
	 */
	rllen = readlink(sourcepath, targetpath, sizeof(targetpath));
	if (rllen < 0)
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not read symbolic link \"%s\": %m",
					   sourcepath));
	if (rllen >= sizeof(targetpath))
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("symbolic link \"%s\" target is too long",
					   sourcepath));
	targetpath[rllen] = '\0';

	return pstrdup(targetpath);
}
