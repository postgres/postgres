/*-------------------------------------------------------------------------
 *
 * compress_io.h
 *	 Interface to compress_io.c routines
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_io.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef __COMPRESS_IO__
#define __COMPRESS_IO__

#include "pg_backup_archiver.h"

/* Initial buffer sizes used in zlib compression. */
#define ZLIB_OUT_SIZE	4096
#define ZLIB_IN_SIZE	4096

typedef enum
{
	COMPR_ALG_NONE,
	COMPR_ALG_LIBZ
} CompressionAlgorithm;

/* Prototype for callback function to WriteDataToArchive() */
typedef void (*WriteFunc) (ArchiveHandle *AH, const char *buf, size_t len);

/*
 * Prototype for callback function to ReadDataFromArchive()
 *
 * ReadDataFromArchive will call the read function repeatedly, until it
 * returns 0 to signal EOF. ReadDataFromArchive passes a buffer to read the
 * data into in *buf, of length *buflen. If that's not big enough for the
 * callback function, it can free() it and malloc() a new one, returning the
 * new buffer and its size in *buf and *buflen.
 *
 * Returns the number of bytes read into *buf, or 0 on EOF.
 */
typedef size_t (*ReadFunc) (ArchiveHandle *AH, char **buf, size_t *buflen);

/* struct definition appears in compress_io.c */
typedef struct CompressorState CompressorState;

extern CompressorState *AllocateCompressor(int compression, WriteFunc writeF);
extern void ReadDataFromArchive(ArchiveHandle *AH, int compression,
								ReadFunc readF);
extern void WriteDataToArchive(ArchiveHandle *AH, CompressorState *cs,
							   const void *data, size_t dLen);
extern void EndCompressor(ArchiveHandle *AH, CompressorState *cs);


typedef struct cfp cfp;

extern cfp *cfopen(const char *path, const char *mode, int compression);
extern cfp *cfopen_read(const char *path, const char *mode);
extern cfp *cfopen_write(const char *path, const char *mode, int compression);
extern int	cfread(void *ptr, int size, cfp *fp);
extern int	cfwrite(const void *ptr, int size, cfp *fp);
extern int	cfgetc(cfp *fp);
extern char *cfgets(cfp *fp, char *buf, int len);
extern int	cfclose(cfp *fp);
extern int	cfeof(cfp *fp);
extern const char *get_cfp_error(cfp *fp);

#endif
