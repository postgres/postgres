/*-------------------------------------------------------------------------
 *
 * fd.h
 *	  Virtual file descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fd.h,v 1.39.4.2 2004/02/23 23:03:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * calls:
 *
 *	File {Close, Read, Write, Seek, Tell, MarkDirty, Sync}
 *	{File Name Open, Allocate, Free} File
 *
 * These are NOT JUST RENAMINGS OF THE UNIX ROUTINES.
 * Use them for all file activity...
 *
 *	File fd;
 *	fd = FilePathOpenFile("foo", O_RDONLY, 0600);
 *
 *	AllocateFile();
 *	FreeFile();
 *
 * Use AllocateFile, not fopen, if you need a stdio file (FILE*); then
 * use FreeFile, not fclose, to close it.  AVOID using stdio for files
 * that you intend to hold open for any length of time, since there is
 * no way for them to share kernel file descriptors with other files.
 *
 * Likewise, use AllocateDir/FreeDir, not opendir/closedir, to allocate
 * open directories (DIR*).
 */
#ifndef FD_H
#define FD_H

#include <dirent.h>


/*
 * FileSeek uses the standard UNIX lseek(2) flags.
 */

typedef char *FileName;

typedef int File;


/* GUC parameter */
extern int	max_files_per_process;


/*
 * prototypes for functions in fd.c
 */

/* Operations on virtual Files --- equivalent to Unix kernel file ops */
extern File FileNameOpenFile(FileName fileName, int fileFlags, int fileMode);
extern File PathNameOpenFile(FileName fileName, int fileFlags, int fileMode);
extern File OpenTemporaryFile(bool interXact);
extern void FileClose(File file);
extern void FileUnlink(File file);
extern int	FileRead(File file, char *buffer, int amount);
extern int	FileWrite(File file, char *buffer, int amount);
extern long FileSeek(File file, long offset, int whence);
extern int	FileTruncate(File file, long offset);

/* Operations that allow use of regular stdio --- USE WITH CAUTION */
extern FILE *AllocateFile(char *name, char *mode);
extern void FreeFile(FILE *file);

/* Operations to allow use of the <dirent.h> library routines */
extern DIR *AllocateDir(const char *dirname);
extern int	FreeDir(DIR *dir);

/* If you've really really gotta have a plain kernel FD, use this */
extern int	BasicOpenFile(FileName fileName, int fileFlags, int fileMode);

/* Miscellaneous support routines */
extern void set_max_safe_fds(void);
extern void closeAllVfds(void);
extern void AtEOXact_Files(void);
extern void RemovePgTempFiles(void);
extern int	pg_fsync(int fd);
extern int	pg_fdatasync(int fd);

#endif   /* FD_H */
