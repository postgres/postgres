/*-------------------------------------------------------------------------
 *
 * archive.h
 *	  Routines to access WAL archives from frontend
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/archive.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FE_ARCHIVE_H
#define FE_ARCHIVE_H

extern int	RestoreArchivedFile(const char *path,
								const char *xlogfname,
								off_t expectedSize,
								const char *restoreCommand);

#endif							/* FE_ARCHIVE_H */
