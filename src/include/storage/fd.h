/*-------------------------------------------------------------------------
 *
 * fd.h
 *	  Virtual file descriptor definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fd.h,v 1.13 1999/05/09 00:52:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * calls:
 *
 *	File {Close, Read, Write, Seek, Tell, Sync}
 *	{File Name Open, Allocate, Free} File
 *
 * These are NOT JUST RENAMINGS OF THE UNIX ROUTINES.
 * Use them for all file activity...
 *
 *	File fd;
 *	fd = FilePathOpenFile("foo", O_RDONLY);
 *
 *	AllocateFile();
 *	FreeFile();
 *
 * Use AllocateFile, not fopen, if you need a stdio file (FILE*); then
 * use FreeFile, not fclose, to close it.  AVOID using stdio for files
 * that you intend to hold open for any length of time, since there is
 * no way for them to share kernel file descriptors with other files.
 *
 * The BufFile routines provide a partial replacement for stdio.  Currently
 * they only support buffered access to a virtual file, without any of
 * stdio's formatting features.  That's enough for immediate needs, but
 * the set of facilities could be expanded if necessary.
 */
#ifndef FD_H
#define FD_H

#include <stdio.h>

/*
 * FileSeek uses the standard UNIX lseek(2) flags.
 */

typedef char *FileName;

typedef int File;

/* BufFile is an opaque type whose details are not known outside fd.c. */

typedef struct BufFile BufFile;

/* why is this here? fd.c doesn't want it ... */
struct pgstat
{								/* just the fields we need from stat
								 * structure */
	int			st_ino;
	int			st_mode;
	unsigned int st_size;
	unsigned int st_sizehigh;	/* high order bits */
/* 2^64 == 1.8 x 10^20 bytes */
	int			st_uid;
	int			st_atime_s;		/* just the seconds */
	int			st_mtime_s;		/* since SysV and the new BSD both have */
	int			st_ctime_s;		/* usec fields.. */
};

/*
 * prototypes for functions in fd.c
 */

/* Operations on virtual Files --- equivalent to Unix kernel file ops */
extern File FileNameOpenFile(FileName fileName, int fileFlags, int fileMode);
extern File PathNameOpenFile(FileName fileName, int fileFlags, int fileMode);
extern File OpenTemporaryFile(void);
extern void FileClose(File file);
extern void FileUnlink(File file);
extern int	FileRead(File file, char *buffer, int amount);
extern int	FileWrite(File file, char *buffer, int amount);
extern long FileSeek(File file, long offset, int whence);
extern int	FileTruncate(File file, int offset);
extern int	FileSync(File file);

/* Operations that allow use of regular stdio --- USE WITH CAUTION */
extern FILE *AllocateFile(char *name, char *mode);
extern void FreeFile(FILE *);

/* Operations on BufFiles --- a very incomplete emulation of stdio
 * atop virtual Files...
 */
extern BufFile *BufFileCreate(File file);
extern void BufFileClose(BufFile *file);
extern size_t BufFileRead(BufFile *file, void *ptr, size_t size);
extern size_t BufFileWrite(BufFile *file, void *ptr, size_t size);
extern int BufFileFlush(BufFile *file);
extern long BufFileSeek(BufFile *file, long offset, int whence);

/* Miscellaneous support routines */
extern int	FileNameUnlink(char *filename);
extern void closeAllVfds(void);
extern void AtEOXact_Files(void);
extern int	pg_fsync(int fd);

#endif	 /* FD_H */
