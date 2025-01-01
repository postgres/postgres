/*-------------------------------------------------------------------------
 *
 * compress_zstd.h
 *	 Zstd interface to compress_io.c routines
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_zstd.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef COMPRESS_ZSTD_H
#define COMPRESS_ZSTD_H

#include "compress_io.h"

extern void InitCompressorZstd(CompressorState *cs,
							   const pg_compress_specification compression_spec);
extern void InitCompressFileHandleZstd(CompressFileHandle *CFH,
									   const pg_compress_specification compression_spec);

#endif							/* COMPRESS_ZSTD_H */
