/*-------------------------------------------------------------------------
 *
 * fd.h--
 *    Virtual file descriptor definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fd.h,v 1.8 1997/08/19 21:39:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * calls:
 * 
 *  File {Close, Read, Write, Seek, Tell, Sync}
 *  {File Name Open, Allocate, Free} File
 *
 * These are NOT JUST RENAMINGS OF THE UNIX ROUTINES.
 * use them for all file activity...
 *
 *  fd = FilePathOpenFile("foo", O_RDONLY);
 *  File fd;
 *
 * use AllocateFile if you need a file descriptor in some other context.
 * it will make sure that there is a file descriptor free
 *
 * use FreeFile to let the virtual file descriptor package know that 
 * there is now a free fd (when you are done with it)
 *
 *  AllocateFile();
 *  FreeFile();
 */
#ifndef	FD_H
#define FD_H

#include <stdio.h>

/*
 * FileSeek uses the standard UNIX lseek(2) flags.
 */

typedef char   *FileName;

typedef int	File;

/* originally in libpq-fs.h */
struct pgstat { /* just the fields we need from stat structure */
    int st_ino;
    int st_mode;
    unsigned int st_size;
    unsigned int st_sizehigh;	/* high order bits */
/* 2^64 == 1.8 x 10^20 bytes */
    int st_uid;
    int st_atime_s;	/* just the seconds */
    int st_mtime_s;	/* since SysV and the new BSD both have */
    int st_ctime_s;	/* usec fields.. */
};

/*
 * prototypes for functions in fd.c
 */
extern File FileNameOpenFile(FileName fileName, int fileFlags, int fileMode);
extern File PathNameOpenFile(FileName fileName, int fileFlags, int fileMode);
extern void FileClose(File file);
extern void FileUnlink(File file);
extern int FileRead(File file, char *buffer, int amount);
extern int FileWrite(File file, char *buffer, int amount);
extern long FileSeek(File file, long offset, int whence);
extern int FileTruncate(File file, int offset);
extern int FileSync(File file);
extern int FileNameUnlink(char *filename);
extern FILE *AllocateFile(char *name, char *mode);
extern void FreeFile(FILE *);
extern void closeAllVfds(void);
extern int pg_fsync(int fd);

#endif	/* FD_H */
