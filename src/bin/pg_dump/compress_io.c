/*-------------------------------------------------------------------------
 *
 * compress_io.c
 *	 Routines for archivers to write an uncompressed or compressed data
 *	 stream.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * This file includes two APIs for dealing with compressed data. The first
 * provides more flexibility, using callbacks to read/write data from the
 * underlying stream. The second API is a wrapper around fopen and
 * friends, providing an interface similar to those, but abstracts away
 * the possible compression. The second API is aimed for the resulting
 * files to be easily manipulated with an external compression utility
 * program.
 *
 * Compressor API
 * --------------
 *
 *	The interface for writing to an archive consists of three functions:
 *	AllocateCompressor, writeData, and EndCompressor. First you call
 *	AllocateCompressor, then write all the data by calling writeData as many
 *	times as needed, and finally EndCompressor. writeData will call the
 *	WriteFunc that was provided to AllocateCompressor for each chunk of
 *	compressed data.
 *
 *	The interface for reading an archive consists of the same three functions:
 *	AllocateCompressor, readData, and EndCompressor. First you call
 *	AllocateCompressor, then read all the data by calling readData to read the
 *	whole compressed stream which repeatedly calls the given ReadFunc. ReadFunc
 *	returns the compressed data one chunk at a time. Then readData decompresses
 *	it and passes the decompressed data to ahwrite(), until ReadFunc returns 0
 *	to signal EOF. The interface is the same for compressed and uncompressed
 *	streams.
 *
 * Compressed stream API
 * ----------------------
 *
 *	The compressed stream API is providing a set of function pointers for
 *	opening, reading, writing, and finally closing files. The implemented
 *	function pointers are documented in the corresponding header file and are
 *	common for all streams. It allows the caller to use the same functions for
 *	both compressed and uncompressed streams.
 *
 *	The interface consists of three functions, InitCompressFileHandle,
 *	InitDiscoverCompressFileHandle, and EndCompressFileHandle. If the
 *	compression is known, then start by calling InitCompressFileHandle,
 *	otherwise discover it by using InitDiscoverCompressFileHandle. Then call
 *	the function pointers as required for the read/write operations. Finally
 *	call EndCompressFileHandle to end the stream.
 *
 *	InitDiscoverCompressFileHandle tries to infer the compression by the
 *	filename suffix. If the suffix is not yet known then it tries to simply
 *	open the file and if it fails, it tries to open the same file with
 *	compressed suffixes (.gz, .lz4 and .zst, in this order).
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_io.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <sys/stat.h>
#include <unistd.h>

#include "compress_gzip.h"
#include "compress_io.h"
#include "compress_lz4.h"
#include "compress_none.h"
#include "compress_zstd.h"

/*----------------------
 * Generic functions
 *----------------------
 */

/*
 * Checks whether support for a compression algorithm is implemented in
 * pg_dump/restore.
 *
 * On success returns NULL, otherwise returns a malloc'ed string which can be
 * used by the caller in an error message.
 */
char *
supports_compression(const pg_compress_specification compression_spec)
{
	const pg_compress_algorithm algorithm = compression_spec.algorithm;
	bool		supported = false;

	if (algorithm == PG_COMPRESSION_NONE)
		supported = true;
#ifdef HAVE_LIBZ
	if (algorithm == PG_COMPRESSION_GZIP)
		supported = true;
#endif
#ifdef USE_LZ4
	if (algorithm == PG_COMPRESSION_LZ4)
		supported = true;
#endif
#ifdef USE_ZSTD
	if (algorithm == PG_COMPRESSION_ZSTD)
		supported = true;
#endif

	if (!supported)
		return psprintf(_("this build does not support compression with %s"),
						get_compress_algorithm_name(algorithm));

	return NULL;
}

/*----------------------
 * Compressor API
 *----------------------
 */

/*
 * Allocate a new compressor.
 */
CompressorState *
AllocateCompressor(const pg_compress_specification compression_spec,
				   ReadFunc readF, WriteFunc writeF)
{
	CompressorState *cs;

	cs = (CompressorState *) pg_malloc0(sizeof(CompressorState));
	cs->readF = readF;
	cs->writeF = writeF;

	if (compression_spec.algorithm == PG_COMPRESSION_NONE)
		InitCompressorNone(cs, compression_spec);
	else if (compression_spec.algorithm == PG_COMPRESSION_GZIP)
		InitCompressorGzip(cs, compression_spec);
	else if (compression_spec.algorithm == PG_COMPRESSION_LZ4)
		InitCompressorLZ4(cs, compression_spec);
	else if (compression_spec.algorithm == PG_COMPRESSION_ZSTD)
		InitCompressorZstd(cs, compression_spec);

	return cs;
}

/*
 * Terminate compression library context and flush its buffers.
 */
void
EndCompressor(ArchiveHandle *AH, CompressorState *cs)
{
	cs->end(AH, cs);
	pg_free(cs);
}

/*----------------------
 * Compressed stream API
 *----------------------
 */

/*
 * Private routines
 */
static int
hasSuffix(const char *filename, const char *suffix)
{
	int			filenamelen = strlen(filename);
	int			suffixlen = strlen(suffix);

	if (filenamelen < suffixlen)
		return 0;

	return memcmp(&filename[filenamelen - suffixlen],
				  suffix,
				  suffixlen) == 0;
}

/* free() without changing errno; useful in several places below */
static void
free_keep_errno(void *p)
{
	int			save_errno = errno;

	free(p);
	errno = save_errno;
}

/*
 * Public interface
 */

/*
 * Initialize a compress file handle for the specified compression algorithm.
 */
CompressFileHandle *
InitCompressFileHandle(const pg_compress_specification compression_spec)
{
	CompressFileHandle *CFH;

	CFH = pg_malloc0(sizeof(CompressFileHandle));

	if (compression_spec.algorithm == PG_COMPRESSION_NONE)
		InitCompressFileHandleNone(CFH, compression_spec);
	else if (compression_spec.algorithm == PG_COMPRESSION_GZIP)
		InitCompressFileHandleGzip(CFH, compression_spec);
	else if (compression_spec.algorithm == PG_COMPRESSION_LZ4)
		InitCompressFileHandleLZ4(CFH, compression_spec);
	else if (compression_spec.algorithm == PG_COMPRESSION_ZSTD)
		InitCompressFileHandleZstd(CFH, compression_spec);

	return CFH;
}

/*
 * Checks if a compressed file (with the specified extension) exists.
 *
 * The filename of the tested file is stored to fname buffer (the existing
 * buffer is freed, new buffer is allocated and returned through the pointer).
 */
static bool
check_compressed_file(const char *path, char **fname, char *ext)
{
	free_keep_errno(*fname);
	*fname = psprintf("%s.%s", path, ext);
	return (access(*fname, F_OK) == 0);
}

/*
 * Open a file for reading. 'path' is the file to open, and 'mode' should
 * be either "r" or "rb".
 *
 * If the file at 'path' contains the suffix of a supported compression method,
 * currently this includes ".gz", ".lz4" and ".zst", then this compression will be used
 * throughout. Otherwise the compression will be inferred by iteratively trying
 * to open the file at 'path', first as is, then by appending known compression
 * suffixes. So if you pass "foo" as 'path', this will open either "foo" or
 * "foo.{gz,lz4,zst}", trying in that order.
 *
 * On failure, return NULL with an error code in errno.
 */
CompressFileHandle *
InitDiscoverCompressFileHandle(const char *path, const char *mode)
{
	CompressFileHandle *CFH = NULL;
	struct stat st;
	char	   *fname;
	pg_compress_specification compression_spec = {0};

	compression_spec.algorithm = PG_COMPRESSION_NONE;

	Assert(strcmp(mode, PG_BINARY_R) == 0);

	fname = pg_strdup(path);

	if (hasSuffix(fname, ".gz"))
		compression_spec.algorithm = PG_COMPRESSION_GZIP;
	else if (hasSuffix(fname, ".lz4"))
		compression_spec.algorithm = PG_COMPRESSION_LZ4;
	else if (hasSuffix(fname, ".zst"))
		compression_spec.algorithm = PG_COMPRESSION_ZSTD;
	else
	{
		if (stat(path, &st) == 0)
			compression_spec.algorithm = PG_COMPRESSION_NONE;
		else if (check_compressed_file(path, &fname, "gz"))
			compression_spec.algorithm = PG_COMPRESSION_GZIP;
		else if (check_compressed_file(path, &fname, "lz4"))
			compression_spec.algorithm = PG_COMPRESSION_LZ4;
		else if (check_compressed_file(path, &fname, "zst"))
			compression_spec.algorithm = PG_COMPRESSION_ZSTD;
	}

	CFH = InitCompressFileHandle(compression_spec);
	if (!CFH->open_func(fname, -1, mode, CFH))
	{
		free_keep_errno(CFH);
		CFH = NULL;
	}
	free_keep_errno(fname);

	return CFH;
}

/*
 * Close an open file handle and release its memory.
 *
 * On failure, returns false and sets errno appropriately.
 */
bool
EndCompressFileHandle(CompressFileHandle *CFH)
{
	bool		ret = false;

	if (CFH->private_data)
		ret = CFH->close_func(CFH);

	free_keep_errno(CFH);

	return ret;
}
