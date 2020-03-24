/*-------------------------------------------------------------------------
 *
 * archive.c
 *	  Common WAL archive routines
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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
#include "lib/stringinfo.h"

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
 * by the caller, then NULL is returned.
 */
char *
BuildRestoreCommand(const char *restoreCommand,
					const char *xlogpath,
					const char *xlogfname,
					const char *lastRestartPointFname)
{
	StringInfoData result;
	const char *sp;

	/*
	 * Build the command to be executed.
	 */
	initStringInfo(&result);

	for (sp = restoreCommand; *sp; sp++)
	{
		if (*sp == '%')
		{
			switch (sp[1])
			{
				case 'p':
					{
						char	   *nativePath;

						/* %p: relative path of target file */
						if (xlogpath == NULL)
						{
							pfree(result.data);
							return NULL;
						}
						sp++;

						/*
						 * This needs to use a placeholder to not modify the
						 * input with the conversion done via
						 * make_native_path().
						 */
						nativePath = pstrdup(xlogpath);
						make_native_path(nativePath);
						appendStringInfoString(&result,
											   nativePath);
						pfree(nativePath);
						break;
					}
				case 'f':
					/* %f: filename of desired file */
					if (xlogfname == NULL)
					{
						pfree(result.data);
						return NULL;
					}
					sp++;
					appendStringInfoString(&result, xlogfname);
					break;
				case 'r':
					/* %r: filename of last restartpoint */
					if (lastRestartPointFname == NULL)
					{
						pfree(result.data);
						return NULL;
					}
					sp++;
					appendStringInfoString(&result,
										   lastRestartPointFname);
					break;
				case '%':
					/* convert %% to a single % */
					sp++;
					appendStringInfoChar(&result, *sp);
					break;
				default:
					/* otherwise treat the % as not special */
					appendStringInfoChar(&result, *sp);
					break;
			}
		}
		else
		{
			appendStringInfoChar(&result, *sp);
		}
	}

	return result.data;
}
