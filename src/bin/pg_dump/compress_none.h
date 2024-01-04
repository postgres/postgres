/*-------------------------------------------------------------------------
 *
 * compress_none.h
 *	 Uncompressed interface to compress_io.c routines
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_none.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _COMPRESS_NONE_H_
#define _COMPRESS_NONE_H_

#include "compress_io.h"

extern void InitCompressorNone(CompressorState *cs,
							   const pg_compress_specification compression_spec);
extern void InitCompressFileHandleNone(CompressFileHandle *CFH,
									   const pg_compress_specification compression_spec);

#endif							/* _COMPRESS_NONE_H_ */
