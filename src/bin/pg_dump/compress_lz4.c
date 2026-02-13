/*-------------------------------------------------------------------------
 *
 * compress_lz4.c
 *	 Routines for archivers to write a LZ4 compressed data stream.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_lz4.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"
#include <unistd.h>

#include "compress_lz4.h"
#include "pg_backup_utils.h"

#ifdef USE_LZ4
#include <lz4frame.h>

/*
 * LZ4F_HEADER_SIZE_MAX first appeared in v1.7.5 of the library.
 * Redefine it for installations with a lesser version.
 */
#ifndef LZ4F_HEADER_SIZE_MAX
#define LZ4F_HEADER_SIZE_MAX	32
#endif

/*---------------------------------
 * Common to both compression APIs
 *---------------------------------
 */

/*
 * (de)compression state used by both the Compressor and Stream APIs.
 */
typedef struct LZ4State
{
	/*
	 * Used by the Stream API to keep track of the file stream.
	 */
	FILE	   *fp;

	LZ4F_preferences_t prefs;

	LZ4F_compressionContext_t ctx;
	LZ4F_decompressionContext_t dtx;

	/*
	 * Used by the Stream API's lazy initialization.
	 */
	bool		inited;

	/*
	 * Used by the Stream API to distinguish between compression and
	 * decompression operations.
	 */
	bool		compressing;

	/*
	 * I/O buffer area.
	 */
	char	   *buffer;			/* buffer for compressed data */
	size_t		buflen;			/* allocated size of buffer */
	size_t		bufdata;		/* amount of valid data currently in buffer */
	/* These fields are used only while decompressing: */
	size_t		bufnext;		/* next buffer position to decompress */
	char	   *outbuf;			/* buffer for decompressed data */
	size_t		outbuflen;		/* allocated size of outbuf */
	size_t		outbufdata;		/* amount of valid data currently in outbuf */
	size_t		outbufnext;		/* next outbuf position to return */

	/*
	 * Used by both APIs to keep track of error codes.
	 */
	size_t		errcode;
} LZ4State;

/*
 * LZ4State_compression_init
 *		Initialize the required LZ4State members for compression.
 *
 * Write the LZ4 frame header in a buffer keeping track of its length. Users of
 * this function can choose when and how to write the header to a file stream.
 *
 * Returns true on success. In case of a failure returns false, and stores the
 * error code in state->errcode.
 */
static bool
LZ4State_compression_init(LZ4State *state)
{
	size_t		status;

	/*
	 * Compute size needed for buffer, assuming we will present at most
	 * DEFAULT_IO_BUFFER_SIZE input bytes at a time.
	 */
	state->buflen = LZ4F_compressBound(DEFAULT_IO_BUFFER_SIZE, &state->prefs);

	/*
	 * Add some slop to ensure we're not forced to flush every time.
	 *
	 * The present slop factor of 50% is chosen so that the typical output
	 * block size is about 128K when DEFAULT_IO_BUFFER_SIZE = 128K.  We might
	 * need a different slop factor to maintain that equivalence if
	 * DEFAULT_IO_BUFFER_SIZE is changed dramatically.
	 */
	state->buflen += state->buflen / 2;

	/*
	 * LZ4F_compressBegin requires a buffer that is greater or equal to
	 * LZ4F_HEADER_SIZE_MAX. Verify that the requirement is met.
	 */
	if (state->buflen < LZ4F_HEADER_SIZE_MAX)
		state->buflen = LZ4F_HEADER_SIZE_MAX;

	status = LZ4F_createCompressionContext(&state->ctx, LZ4F_VERSION);
	if (LZ4F_isError(status))
	{
		state->errcode = status;
		return false;
	}

	state->buffer = pg_malloc(state->buflen);

	/*
	 * Insert LZ4 header into buffer.
	 */
	status = LZ4F_compressBegin(state->ctx,
								state->buffer, state->buflen,
								&state->prefs);
	if (LZ4F_isError(status))
	{
		state->errcode = status;
		return false;
	}

	state->bufdata = status;

	return true;
}

/*----------------------
 * Compressor API
 *----------------------
 */

/* Private routines that support LZ4 compressed data I/O */

static void
ReadDataFromArchiveLZ4(ArchiveHandle *AH, CompressorState *cs)
{
	size_t		r;
	size_t		readbuflen;
	char	   *outbuf;
	char	   *readbuf;
	LZ4F_decompressionContext_t ctx = NULL;
	LZ4F_decompressOptions_t dec_opt;
	LZ4F_errorCode_t status;

	memset(&dec_opt, 0, sizeof(dec_opt));
	status = LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);
	if (LZ4F_isError(status))
		pg_fatal("could not create LZ4 decompression context: %s",
				 LZ4F_getErrorName(status));

	outbuf = pg_malloc(DEFAULT_IO_BUFFER_SIZE);
	readbuf = pg_malloc(DEFAULT_IO_BUFFER_SIZE);
	readbuflen = DEFAULT_IO_BUFFER_SIZE;
	while ((r = cs->readF(AH, &readbuf, &readbuflen)) > 0)
	{
		char	   *readp;
		char	   *readend;

		/* Process one chunk */
		readp = readbuf;
		readend = readbuf + r;
		while (readp < readend)
		{
			size_t		out_size = DEFAULT_IO_BUFFER_SIZE;
			size_t		read_size = readend - readp;

			status = LZ4F_decompress(ctx, outbuf, &out_size,
									 readp, &read_size, &dec_opt);
			if (LZ4F_isError(status))
				pg_fatal("could not decompress: %s",
						 LZ4F_getErrorName(status));

			ahwrite(outbuf, 1, out_size, AH);
			readp += read_size;
		}
	}

	pg_free(outbuf);
	pg_free(readbuf);

	status = LZ4F_freeDecompressionContext(ctx);
	if (LZ4F_isError(status))
		pg_fatal("could not free LZ4 decompression context: %s",
				 LZ4F_getErrorName(status));
}

static void
WriteDataToArchiveLZ4(ArchiveHandle *AH, CompressorState *cs,
					  const void *data, size_t dLen)
{
	LZ4State   *state = (LZ4State *) cs->private_data;
	size_t		remaining = dLen;

	while (remaining > 0)
	{
		size_t		chunk;
		size_t		required;
		size_t		status;

		/* We don't try to present more than DEFAULT_IO_BUFFER_SIZE bytes */
		chunk = Min(remaining, (size_t) DEFAULT_IO_BUFFER_SIZE);

		/* If not enough space, must flush buffer */
		required = LZ4F_compressBound(chunk, &state->prefs);
		if (required > state->buflen - state->bufdata)
		{
			cs->writeF(AH, state->buffer, state->bufdata);
			state->bufdata = 0;
		}

		status = LZ4F_compressUpdate(state->ctx,
									 state->buffer + state->bufdata,
									 state->buflen - state->bufdata,
									 data, chunk, NULL);

		if (LZ4F_isError(status))
			pg_fatal("could not compress data: %s",
					 LZ4F_getErrorName(status));

		state->bufdata += status;

		data = ((const char *) data) + chunk;
		remaining -= chunk;
	}
}

static void
EndCompressorLZ4(ArchiveHandle *AH, CompressorState *cs)
{
	LZ4State   *state = (LZ4State *) cs->private_data;
	size_t		required;
	size_t		status;

	/* Nothing needs to be done */
	if (!state)
		return;

	/* We might need to flush the buffer to make room for LZ4F_compressEnd */
	required = LZ4F_compressBound(0, &state->prefs);
	if (required > state->buflen - state->bufdata)
	{
		cs->writeF(AH, state->buffer, state->bufdata);
		state->bufdata = 0;
	}

	status = LZ4F_compressEnd(state->ctx,
							  state->buffer + state->bufdata,
							  state->buflen - state->bufdata,
							  NULL);
	if (LZ4F_isError(status))
		pg_fatal("could not end compression: %s",
				 LZ4F_getErrorName(status));
	state->bufdata += status;

	/* Write the final bufferload */
	cs->writeF(AH, state->buffer, state->bufdata);

	status = LZ4F_freeCompressionContext(state->ctx);
	if (LZ4F_isError(status))
		pg_fatal("could not end compression: %s",
				 LZ4F_getErrorName(status));

	pg_free(state->buffer);
	pg_free(state);

	cs->private_data = NULL;
}

/*
 * Public routines that support LZ4 compressed data I/O
 */
void
InitCompressorLZ4(CompressorState *cs, const pg_compress_specification compression_spec)
{
	LZ4State   *state;

	cs->readData = ReadDataFromArchiveLZ4;
	cs->writeData = WriteDataToArchiveLZ4;
	cs->end = EndCompressorLZ4;

	cs->compression_spec = compression_spec;

	/*
	 * Read operations have access to the whole input. No state needs to be
	 * carried between calls.
	 */
	if (cs->readF)
		return;

	state = pg_malloc0_object(LZ4State);
	if (cs->compression_spec.level >= 0)
		state->prefs.compressionLevel = cs->compression_spec.level;

	if (!LZ4State_compression_init(state))
		pg_fatal("could not initialize LZ4 compression: %s",
				 LZ4F_getErrorName(state->errcode));

	cs->private_data = state;
}

/*----------------------
 * Compress Stream API
 *----------------------
 */


/*
 * LZ4 equivalent to feof() or gzeof().  Return true iff there is no
 * more buffered data and the end of the input file has been reached.
 */
static bool
LZ4Stream_eof(CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;

	return state->outbufnext >= state->outbufdata &&
		state->bufnext >= state->bufdata &&
		feof(state->fp);
}

static const char *
LZ4Stream_get_error(CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;
	const char *errmsg;

	if (LZ4F_isError(state->errcode))
		errmsg = LZ4F_getErrorName(state->errcode);
	else
		errmsg = strerror(errno);

	return errmsg;
}

/*
 * Initialize an already alloc'ed LZ4State struct for subsequent calls.
 *
 * Creates the necessary contexts for either compression or decompression. When
 * compressing data (indicated by compressing=true), it additionally writes the
 * LZ4 header in the output buffer.
 *
 * It's expected that a not-yet-initialized LZ4State will be zero-filled.
 *
 * Returns true on success. In case of a failure returns false, and stores the
 * error code in state->errcode.
 */
static bool
LZ4Stream_init(LZ4State *state, bool compressing)
{
	size_t		status;

	if (state->inited)
		return true;

	state->compressing = compressing;

	if (state->compressing)
	{
		if (!LZ4State_compression_init(state))
			return false;
	}
	else
	{
		status = LZ4F_createDecompressionContext(&state->dtx, LZ4F_VERSION);
		if (LZ4F_isError(status))
		{
			state->errcode = status;
			return false;
		}

		state->buflen = DEFAULT_IO_BUFFER_SIZE;
		state->buffer = pg_malloc(state->buflen);
		state->outbuflen = DEFAULT_IO_BUFFER_SIZE;
		state->outbuf = pg_malloc(state->outbuflen);
	}

	state->inited = true;
	return true;
}

/*
 * The workhorse for reading decompressed content out of an LZ4 compressed
 * stream.
 *
 * It will read up to 'ptrsize' decompressed content, or up to the new line
 * char if one is found first when the eol_flag is set.
 *
 * Returns the number of bytes of decompressed data copied into the ptr
 * buffer, or -1 in case of error.
 */
static int
LZ4Stream_read_internal(LZ4State *state, void *ptr, int ptrsize, bool eol_flag)
{
	int			dsize = 0;
	int			remaining = ptrsize;

	/* Lazy init */
	if (!LZ4Stream_init(state, false /* decompressing */ ))
	{
		pg_log_error("unable to initialize LZ4 library: %s",
					 LZ4F_getErrorName(state->errcode));
		return -1;
	}

	/* Loop until postcondition is satisfied */
	while (remaining > 0)
	{
		/*
		 * If we already have some decompressed data, return that.
		 */
		if (state->outbufnext < state->outbufdata)
		{
			char	   *outptr = state->outbuf + state->outbufnext;
			size_t		readlen = state->outbufdata - state->outbufnext;
			bool		eol_found = false;

			if (readlen > remaining)
				readlen = remaining;
			/* If eol_flag is set, don't read beyond a newline */
			if (eol_flag)
			{
				char	   *eolptr = memchr(outptr, '\n', readlen);

				if (eolptr)
				{
					readlen = eolptr - outptr + 1;
					eol_found = true;
				}
			}
			memcpy(ptr, outptr, readlen);
			ptr = ((char *) ptr) + readlen;
			state->outbufnext += readlen;
			dsize += readlen;
			remaining -= readlen;
			if (eol_found || remaining == 0)
				break;
			/* We must have emptied outbuf */
			Assert(state->outbufnext >= state->outbufdata);
		}

		/*
		 * If we don't have any pending compressed data, load more into
		 * state->buffer.
		 */
		if (state->bufnext >= state->bufdata)
		{
			size_t		rsize;

			rsize = fread(state->buffer, 1, state->buflen, state->fp);
			if (rsize < state->buflen && !feof(state->fp))
			{
				pg_log_error("could not read from input file: %m");
				return -1;
			}
			if (rsize == 0)
				break;			/* must be EOF */
			state->bufdata = rsize;
			state->bufnext = 0;
		}

		/*
		 * Decompress some data into state->outbuf.
		 */
		{
			size_t		status;
			size_t		outlen = state->outbuflen;
			size_t		inlen = state->bufdata - state->bufnext;

			status = LZ4F_decompress(state->dtx,
									 state->outbuf, &outlen,
									 state->buffer + state->bufnext,
									 &inlen,
									 NULL);
			if (LZ4F_isError(status))
			{
				state->errcode = status;
				pg_log_error("could not read from input file: %s",
							 LZ4F_getErrorName(state->errcode));
				return -1;
			}
			state->bufnext += inlen;
			state->outbufdata = outlen;
			state->outbufnext = 0;
		}
	}

	return dsize;
}

/*
 * Compress size bytes from ptr and write them to the stream.
 */
static void
LZ4Stream_write(const void *ptr, size_t size, CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;
	size_t		remaining = size;

	/* Lazy init */
	if (!LZ4Stream_init(state, true))
		pg_fatal("unable to initialize LZ4 library: %s",
				 LZ4F_getErrorName(state->errcode));

	while (remaining > 0)
	{
		size_t		chunk;
		size_t		required;
		size_t		status;

		/* We don't try to present more than DEFAULT_IO_BUFFER_SIZE bytes */
		chunk = Min(remaining, (size_t) DEFAULT_IO_BUFFER_SIZE);

		/* If not enough space, must flush buffer */
		required = LZ4F_compressBound(chunk, &state->prefs);
		if (required > state->buflen - state->bufdata)
		{
			errno = 0;
			if (fwrite(state->buffer, 1, state->bufdata, state->fp) != state->bufdata)
			{
				errno = (errno) ? errno : ENOSPC;
				pg_fatal("error during writing: %m");
			}
			state->bufdata = 0;
		}

		status = LZ4F_compressUpdate(state->ctx,
									 state->buffer + state->bufdata,
									 state->buflen - state->bufdata,
									 ptr, chunk, NULL);
		if (LZ4F_isError(status))
			pg_fatal("error during writing: %s", LZ4F_getErrorName(status));
		state->bufdata += status;

		ptr = ((const char *) ptr) + chunk;
		remaining -= chunk;
	}
}

/*
 * fread() equivalent implementation for LZ4 compressed files.
 */
static size_t
LZ4Stream_read(void *ptr, size_t size, CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;
	int			ret;

	if ((ret = LZ4Stream_read_internal(state, ptr, size, false)) < 0)
		pg_fatal("could not read from input file: %s", LZ4Stream_get_error(CFH));

	return (size_t) ret;
}

/*
 * fgetc() equivalent implementation for LZ4 compressed files.
 */
static int
LZ4Stream_getc(CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;
	unsigned char c;

	if (LZ4Stream_read_internal(state, &c, 1, false) <= 0)
	{
		if (!LZ4Stream_eof(CFH))
			pg_fatal("could not read from input file: %s", LZ4Stream_get_error(CFH));
		else
			pg_fatal("could not read from input file: end of file");
	}

	return c;
}

/*
 * fgets() equivalent implementation for LZ4 compressed files.
 */
static char *
LZ4Stream_gets(char *ptr, int size, CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;
	int			ret;

	ret = LZ4Stream_read_internal(state, ptr, size - 1, true);

	/*
	 * LZ4Stream_read_internal returning 0 or -1 means that it was either an
	 * EOF or an error, but gets_func is defined to return NULL in either case
	 * so we can treat both the same here.
	 */
	if (ret <= 0)
		return NULL;

	/*
	 * Our caller expects the return string to be NULL terminated and we know
	 * that ret is greater than zero.
	 */
	ptr[ret - 1] = '\0';

	return ptr;
}

/*
 * Finalize (de)compression of a stream. When compressing it will write any
 * remaining content and/or generated footer from the LZ4 API.
 */
static bool
LZ4Stream_close(CompressFileHandle *CFH)
{
	FILE	   *fp;
	LZ4State   *state = (LZ4State *) CFH->private_data;
	size_t		required;
	size_t		status;
	int			ret;

	fp = state->fp;
	if (state->inited)
	{
		if (state->compressing)
		{
			/* We might need to flush the buffer to make room */
			required = LZ4F_compressBound(0, &state->prefs);
			if (required > state->buflen - state->bufdata)
			{
				errno = 0;
				if (fwrite(state->buffer, 1, state->bufdata, state->fp) != state->bufdata)
				{
					errno = (errno) ? errno : ENOSPC;
					pg_log_error("could not write to output file: %m");
				}
				state->bufdata = 0;
			}

			status = LZ4F_compressEnd(state->ctx,
									  state->buffer + state->bufdata,
									  state->buflen - state->bufdata,
									  NULL);
			if (LZ4F_isError(status))
			{
				pg_log_error("could not end compression: %s",
							 LZ4F_getErrorName(status));
			}
			else
				state->bufdata += status;

			errno = 0;
			if (fwrite(state->buffer, 1, state->bufdata, state->fp) != state->bufdata)
			{
				errno = (errno) ? errno : ENOSPC;
				pg_log_error("could not write to output file: %m");
			}

			status = LZ4F_freeCompressionContext(state->ctx);
			if (LZ4F_isError(status))
				pg_log_error("could not end compression: %s",
							 LZ4F_getErrorName(status));
		}
		else
		{
			status = LZ4F_freeDecompressionContext(state->dtx);
			if (LZ4F_isError(status))
				pg_log_error("could not end decompression: %s",
							 LZ4F_getErrorName(status));
			pg_free(state->outbuf);
		}

		pg_free(state->buffer);
	}

	pg_free(state);
	CFH->private_data = NULL;

	errno = 0;
	ret = fclose(fp);
	if (ret != 0)
	{
		pg_log_error("could not close file: %m");
		return false;
	}

	return true;
}

static bool
LZ4Stream_open(const char *path, int fd, const char *mode,
			   CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;

	if (fd >= 0)
		state->fp = fdopen(dup(fd), mode);
	else
		state->fp = fopen(path, mode);
	if (state->fp == NULL)
	{
		state->errcode = errno;
		return false;
	}

	return true;
}

static bool
LZ4Stream_open_write(const char *path, const char *mode, CompressFileHandle *CFH)
{
	char	   *fname;
	int			save_errno;
	bool		ret;

	fname = psprintf("%s.lz4", path);
	ret = CFH->open_func(fname, -1, mode, CFH);

	save_errno = errno;
	pg_free(fname);
	errno = save_errno;

	return ret;
}

/*
 * Public routines
 */
void
InitCompressFileHandleLZ4(CompressFileHandle *CFH,
						  const pg_compress_specification compression_spec)
{
	LZ4State   *state;

	CFH->open_func = LZ4Stream_open;
	CFH->open_write_func = LZ4Stream_open_write;
	CFH->read_func = LZ4Stream_read;
	CFH->write_func = LZ4Stream_write;
	CFH->gets_func = LZ4Stream_gets;
	CFH->getc_func = LZ4Stream_getc;
	CFH->eof_func = LZ4Stream_eof;
	CFH->close_func = LZ4Stream_close;
	CFH->get_error_func = LZ4Stream_get_error;

	CFH->compression_spec = compression_spec;
	state = pg_malloc0_object(LZ4State);
	if (CFH->compression_spec.level >= 0)
		state->prefs.compressionLevel = CFH->compression_spec.level;

	CFH->private_data = state;
}
#else							/* USE_LZ4 */
void
InitCompressorLZ4(CompressorState *cs,
				  const pg_compress_specification compression_spec)
{
	pg_fatal("this build does not support compression with %s", "LZ4");
}

void
InitCompressFileHandleLZ4(CompressFileHandle *CFH,
						  const pg_compress_specification compression_spec)
{
	pg_fatal("this build does not support compression with %s", "LZ4");
}
#endif							/* USE_LZ4 */
