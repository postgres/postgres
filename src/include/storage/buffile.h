/*-------------------------------------------------------------------------
 *
 * buffile.h
 *	  Management of large buffered temporary files.
 *
 * The BufFile routines provide a partial replacement for stdio atop
 * virtual file descriptors managed by fd.c.  Currently they only support
 * buffered access to a virtual file, without any of stdio's formatting
 * features.  That's enough for immediate needs, but the set of facilities
 * could be expanded if necessary.
 *
 * BufFile also supports working with temporary files that exceed the OS
 * file size limit and/or the largest offset representable in an int.
 * It might be better to split that out as a separately accessible module,
 * but currently we have no need for oversize temp files without buffered
 * access.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buffile.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef BUFFILE_H
#define BUFFILE_H

#include "storage/fileset.h"

/* BufFile is an opaque type whose details are not known outside buffile.c. */

typedef struct BufFile BufFile;

/*
 * prototypes for functions in buffile.c
 */

extern BufFile *BufFileCreateTemp(bool interXact);
extern void BufFileClose(BufFile *file);
pg_nodiscard extern size_t BufFileRead(BufFile *file, void *ptr, size_t size);
extern void BufFileReadExact(BufFile *file, void *ptr, size_t size);
extern size_t BufFileReadMaybeEOF(BufFile *file, void *ptr, size_t size, bool eofOK);
extern void BufFileWrite(BufFile *file, const void *ptr, size_t size);
extern int	BufFileSeek(BufFile *file, int fileno, off_t offset, int whence);
extern void BufFileTell(BufFile *file, int *fileno, off_t *offset);
extern int	BufFileSeekBlock(BufFile *file, int64 blknum);
extern int64 BufFileSize(BufFile *file);
extern int64 BufFileAppend(BufFile *target, BufFile *source);

extern BufFile *BufFileCreateFileSet(FileSet *fileset, const char *name);
extern void BufFileExportFileSet(BufFile *file);
extern BufFile *BufFileOpenFileSet(FileSet *fileset, const char *name,
								   int mode, bool missing_ok);
extern void BufFileDeleteFileSet(FileSet *fileset, const char *name,
								 bool missing_ok);
extern void BufFileTruncateFileSet(BufFile *file, int fileno, off_t offset);

#endif							/* BUFFILE_H */
