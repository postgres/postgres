/*-------------------------------------------------------------------------
 *
 * archive.c
 *	  Common WAL archive routines
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/archive.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/archive.h"
#include "common/percentrepl.h"

/*
 * BuildRestoreCommand
 *
 * Builds a restore command to retrieve a file from WAL archives, replacing
 * the supported aliases with values supplied by the caller as defined by
 * the GUC parameter restore_command: xlogpath for %p, xlogfname for %f and
 * lastRestartPointFname for %r.
 *
 * The result is a palloc'd string for the restore command built.  The
 * caller is responsible for freeing it.  If any of the required arguments
 * is NULL and that the corresponding alias is found in the command given
 * by the caller, then an error is thrown.
 */
char *
BuildRestoreCommand(const char *restoreCommand,
					const char *xlogpath,
					const char *xlogfname,
					const char *lastRestartPointFname)
{
	char	   *nativePath = NULL;
	char	   *result;

	if (xlogpath)
	{
		nativePath = pstrdup(xlogpath);
		make_native_path(nativePath);
	}

	result = replace_percent_placeholders(restoreCommand, "restore_command", "frp",
										  xlogfname, lastRestartPointFname, nativePath);

	if (nativePath)
		pfree(nativePath);

	return result;
}
