/*-------------------------------------------------------------------------
 *
 * archive.h
 *	  Common WAL archive routines
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/archive.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ARCHIVE_H
#define ARCHIVE_H

extern char *BuildRestoreCommand(const char *restoreCommand,
								 const char *xlogpath,	/* %p */
								 const char *xlogfname, /* %f */
								 const char *lastRestartPointFname);	/* %r */

#endif							/* ARCHIVE_H */
