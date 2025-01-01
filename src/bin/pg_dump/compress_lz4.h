/*-------------------------------------------------------------------------
 *
 * compress_lz4.h
 *	 LZ4 interface to compress_io.c routines
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_lz4.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _COMPRESS_LZ4_H_
#define _COMPRESS_LZ4_H_

#include "compress_io.h"

extern void InitCompressorLZ4(CompressorState *cs,
							  const pg_compress_specification compression_spec);
extern void InitCompressFileHandleLZ4(CompressFileHandle *CFH,
									  const pg_compress_specification compression_spec);

#endif							/* _COMPRESS_LZ4_H_ */
