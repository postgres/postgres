/*-------------------------------------------------------------------------
 *
 * compress_io.h
 *   Interface to compress_io.c routines
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *     src/bin/pg_dump/compress_io.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef __COMPRESS_IO__
#define __COMPRESS_IO__

#include "postgres_fe.h"
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
typedef size_t (*WriteFunc)(ArchiveHandle *AH, const char *buf, size_t len);

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
typedef size_t (*ReadFunc)(ArchiveHandle *AH, char **buf, size_t *buflen);

/* struct definition appears in compress_io.c */
typedef struct CompressorState CompressorState;

extern CompressorState *AllocateCompressor(int compression, WriteFunc writeF);
extern void ReadDataFromArchive(ArchiveHandle *AH, int compression,
								ReadFunc readF);
extern size_t WriteDataToArchive(ArchiveHandle *AH, CompressorState *cs,
								 const void *data, size_t dLen);
extern void EndCompressor(ArchiveHandle *AH, CompressorState *cs);

#endif
