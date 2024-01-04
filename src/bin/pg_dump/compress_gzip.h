/*-------------------------------------------------------------------------
 *
 * compress_gzip.h
 *	 GZIP interface to compress_io.c routines
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_gzip.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _COMPRESS_GZIP_H_
#define _COMPRESS_GZIP_H_

#include "compress_io.h"

extern void InitCompressorGzip(CompressorState *cs,
							   const pg_compress_specification compression_spec);
extern void InitCompressFileHandleGzip(CompressFileHandle *CFH,
									   const pg_compress_specification compression_spec);

#endif							/* _COMPRESS_GZIP_H_ */
