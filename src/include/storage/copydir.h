/*-------------------------------------------------------------------------
 *
 * copydir.h
 *	  Copy a directory.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/copydir.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPYDIR_H
#define COPYDIR_H

typedef enum FileCopyMethod
{
	FILE_COPY_METHOD_COPY,
	FILE_COPY_METHOD_CLONE,
} FileCopyMethod;

/* GUC parameters */
extern PGDLLIMPORT int file_copy_method;

extern void copydir(const char *fromdir, const char *todir, bool recurse);
extern void copy_file(const char *fromfile, const char *tofile);

#endif							/* COPYDIR_H */
