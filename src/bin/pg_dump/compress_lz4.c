/*-------------------------------------------------------------------------
 *
 * compress_lz4.c
 *	 Routines for archivers to write a LZ4 compressed data stream.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_lz4.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

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
	 * Used by the Compressor API to mark if the compression headers have been
	 * written after initialization.
	 */
	bool		needs_header_flush;

	size_t		buflen;
	char	   *buffer;

	/*
	 * Used by the Stream API to store already uncompressed data that the
	 * caller has not consumed.
	 */
	size_t		overflowalloclen;
	size_t		overflowlen;
	char	   *overflowbuf;

	/*
	 * Used by both APIs to keep track of the compressed data length stored in
	 * the buffer.
	 */
	size_t		compressedlen;

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

	state->buflen = LZ4F_compressBound(DEFAULT_IO_BUFFER_SIZE, &state->prefs);

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
	status = LZ4F_compressBegin(state->ctx,
								state->buffer, state->buflen,
								&state->prefs);
	if (LZ4F_isError(status))
	{
		state->errcode = status;
		return false;
	}

	state->compressedlen = status;

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

	outbuf = pg_malloc0(DEFAULT_IO_BUFFER_SIZE);
	readbuf = pg_malloc0(DEFAULT_IO_BUFFER_SIZE);
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

			memset(outbuf, 0, DEFAULT_IO_BUFFER_SIZE);
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
	size_t		status;
	size_t		chunk;

	/* Write the header if not yet written. */
	if (state->needs_header_flush)
	{
		cs->writeF(AH, state->buffer, state->compressedlen);
		state->needs_header_flush = false;
	}

	while (remaining > 0)
	{

		if (remaining > DEFAULT_IO_BUFFER_SIZE)
			chunk = DEFAULT_IO_BUFFER_SIZE;
		else
			chunk = remaining;

		remaining -= chunk;
		status = LZ4F_compressUpdate(state->ctx,
									 state->buffer, state->buflen,
									 data, chunk, NULL);

		if (LZ4F_isError(status))
			pg_fatal("could not compress data: %s",
					 LZ4F_getErrorName(status));

		cs->writeF(AH, state->buffer, status);

		data = ((char *) data) + chunk;
	}
}

static void
EndCompressorLZ4(ArchiveHandle *AH, CompressorState *cs)
{
	LZ4State   *state = (LZ4State *) cs->private_data;
	size_t		status;

	/* Nothing needs to be done */
	if (!state)
		return;

	/*
	 * Write the header if not yet written. The caller is not required to call
	 * writeData if the relation does not contain any data. Thus it is
	 * possible to reach here without having flushed the header. Do it before
	 * ending the compression.
	 */
	if (state->needs_header_flush)
		cs->writeF(AH, state->buffer, state->compressedlen);

	status = LZ4F_compressEnd(state->ctx,
							  state->buffer, state->buflen,
							  NULL);
	if (LZ4F_isError(status))
		pg_fatal("could not end compression: %s",
				 LZ4F_getErrorName(status));

	cs->writeF(AH, state->buffer, status);

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

	state = pg_malloc0(sizeof(*state));
	if (cs->compression_spec.level >= 0)
		state->prefs.compressionLevel = cs->compression_spec.level;

	if (!LZ4State_compression_init(state))
		pg_fatal("could not initialize LZ4 compression: %s",
				 LZ4F_getErrorName(state->errcode));

	/* Remember that the header has not been written. */
	state->needs_header_flush = true;
	cs->private_data = state;
}

/*----------------------
 * Compress Stream API
 *----------------------
 */


/*
 * LZ4 equivalent to feof() or gzeof().  Return true iff there is no
 * decompressed output in the overflow buffer and the end of the backing file
 * is reached.
 */
static bool
LZ4Stream_eof(CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;

	return state->overflowlen == 0 && feof(state->fp);
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
 * LZ4 header in the output stream.
 *
 * Returns true on success. In case of a failure returns false, and stores the
 * error code in state->errcode.
 */
static bool
LZ4Stream_init(LZ4State *state, int size, bool compressing)
{
	size_t		status;

	if (state->inited)
		return true;

	state->compressing = compressing;
	state->inited = true;

	/* When compressing, write LZ4 header to the output stream. */
	if (state->compressing)
	{

		if (!LZ4State_compression_init(state))
			return false;

		if (fwrite(state->buffer, 1, state->compressedlen, state->fp) != state->compressedlen)
		{
			errno = (errno) ? errno : ENOSPC;
			return false;
		}
	}
	else
	{
		status = LZ4F_createDecompressionContext(&state->dtx, LZ4F_VERSION);
		if (LZ4F_isError(status))
		{
			state->errcode = status;
			return false;
		}

		state->buflen = Max(size, DEFAULT_IO_BUFFER_SIZE);
		state->buffer = pg_malloc(state->buflen);

		state->overflowalloclen = state->buflen;
		state->overflowbuf = pg_malloc(state->overflowalloclen);
		state->overflowlen = 0;
	}

	return true;
}

/*
 * Read already decompressed content from the overflow buffer into 'ptr' up to
 * 'size' bytes, if available. If the eol_flag is set, then stop at the first
 * occurrence of the newline char prior to 'size' bytes.
 *
 * Any unread content in the overflow buffer is moved to the beginning.
 *
 * Returns the number of bytes read from the overflow buffer (and copied into
 * the 'ptr' buffer), or 0 if the overflow buffer is empty.
 */
static int
LZ4Stream_read_overflow(LZ4State *state, void *ptr, int size, bool eol_flag)
{
	char	   *p;
	int			readlen = 0;

	if (state->overflowlen == 0)
		return 0;

	if (state->overflowlen >= size)
		readlen = size;
	else
		readlen = state->overflowlen;

	if (eol_flag && (p = memchr(state->overflowbuf, '\n', readlen)))
		/* Include the line terminating char */
		readlen = p - state->overflowbuf + 1;

	memcpy(ptr, state->overflowbuf, readlen);
	state->overflowlen -= readlen;

	if (state->overflowlen > 0)
		memmove(state->overflowbuf, state->overflowbuf + readlen, state->overflowlen);

	return readlen;
}

/*
 * The workhorse for reading decompressed content out of an LZ4 compressed
 * stream.
 *
 * It will read up to 'ptrsize' decompressed content, or up to the new line
 * char if found first when the eol_flag is set. It is possible that the
 * decompressed output generated by reading any compressed input via the
 * LZ4F API, exceeds 'ptrsize'. Any exceeding decompressed content is stored
 * at an overflow buffer within LZ4State. Of course, when the function is
 * called, it will first try to consume any decompressed content already
 * present in the overflow buffer, before decompressing new content.
 *
 * Returns the number of bytes of decompressed data copied into the ptr
 * buffer, or -1 in case of error.
 */
static int
LZ4Stream_read_internal(LZ4State *state, void *ptr, int ptrsize, bool eol_flag)
{
	int			dsize = 0;
	int			rsize;
	int			size = ptrsize;
	bool		eol_found = false;

	void	   *readbuf;

	/* Lazy init */
	if (!LZ4Stream_init(state, size, false /* decompressing */ ))
		return -1;

	/* No work needs to be done for a zero-sized output buffer */
	if (size <= 0)
		return 0;

	/* Verify that there is enough space in the outbuf */
	if (size > state->buflen)
	{
		state->buflen = size;
		state->buffer = pg_realloc(state->buffer, size);
	}

	/* use already decompressed content if available */
	dsize = LZ4Stream_read_overflow(state, ptr, size, eol_flag);
	if (dsize == size || (eol_flag && memchr(ptr, '\n', dsize)))
		return dsize;

	readbuf = pg_malloc(size);

	do
	{
		char	   *rp;
		char	   *rend;

		rsize = fread(readbuf, 1, size, state->fp);
		if (rsize < size && !feof(state->fp))
			return -1;

		rp = (char *) readbuf;
		rend = (char *) readbuf + rsize;

		while (rp < rend)
		{
			size_t		status;
			size_t		outlen = state->buflen;
			size_t		read_remain = rend - rp;

			memset(state->buffer, 0, outlen);
			status = LZ4F_decompress(state->dtx, state->buffer, &outlen,
									 rp, &read_remain, NULL);
			if (LZ4F_isError(status))
			{
				state->errcode = status;
				return -1;
			}

			rp += read_remain;

			/*
			 * fill in what space is available in ptr if the eol flag is set,
			 * either skip if one already found or fill up to EOL if present
			 * in the outbuf
			 */
			if (outlen > 0 && dsize < size && eol_found == false)
			{
				char	   *p;
				size_t		lib = (!eol_flag) ? size - dsize : size - 1 - dsize;
				size_t		len = outlen < lib ? outlen : lib;

				if (eol_flag &&
					(p = memchr(state->buffer, '\n', outlen)) &&
					(size_t) (p - state->buffer + 1) <= len)
				{
					len = p - state->buffer + 1;
					eol_found = true;
				}

				memcpy((char *) ptr + dsize, state->buffer, len);
				dsize += len;

				/* move what did not fit, if any, at the beginning of the buf */
				if (len < outlen)
					memmove(state->buffer, state->buffer + len, outlen - len);
				outlen -= len;
			}

			/* if there is available output, save it */
			if (outlen > 0)
			{
				while (state->overflowlen + outlen > state->overflowalloclen)
				{
					state->overflowalloclen *= 2;
					state->overflowbuf = pg_realloc(state->overflowbuf,
													state->overflowalloclen);
				}

				memcpy(state->overflowbuf + state->overflowlen, state->buffer, outlen);
				state->overflowlen += outlen;
			}
		}
	} while (rsize == size && dsize < size && eol_found == false);

	pg_free(readbuf);

	return dsize;
}

/*
 * Compress size bytes from ptr and write them to the stream.
 */
static bool
LZ4Stream_write(const void *ptr, size_t size, CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;
	size_t		status;
	int			remaining = size;

	/* Lazy init */
	if (!LZ4Stream_init(state, size, true))
		return false;

	while (remaining > 0)
	{
		int			chunk = Min(remaining, DEFAULT_IO_BUFFER_SIZE);

		remaining -= chunk;

		status = LZ4F_compressUpdate(state->ctx, state->buffer, state->buflen,
									 ptr, chunk, NULL);
		if (LZ4F_isError(status))
		{
			state->errcode = status;
			return false;
		}

		if (fwrite(state->buffer, 1, status, state->fp) != status)
		{
			errno = (errno) ? errno : ENOSPC;
			return false;
		}

		ptr = ((const char *) ptr) + chunk;
	}

	return true;
}

/*
 * fread() equivalent implementation for LZ4 compressed files.
 */
static bool
LZ4Stream_read(void *ptr, size_t size, size_t *rsize, CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;
	int			ret;

	if ((ret = LZ4Stream_read_internal(state, ptr, size, false)) < 0)
		pg_fatal("could not read from input file: %s", LZ4Stream_get_error(CFH));

	if (rsize)
		*rsize = (size_t) ret;

	return true;
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
	if (ret < 0 || (ret == 0 && !LZ4Stream_eof(CFH)))
		pg_fatal("could not read from input file: %s", LZ4Stream_get_error(CFH));

	/* Done reading */
	if (ret == 0)
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
	size_t		status;

	fp = state->fp;
	if (state->inited)
	{
		if (state->compressing)
		{
			status = LZ4F_compressEnd(state->ctx, state->buffer, state->buflen, NULL);
			if (LZ4F_isError(status))
				pg_fatal("could not end compression: %s",
						 LZ4F_getErrorName(status));
			else if (fwrite(state->buffer, 1, status, state->fp) != status)
			{
				errno = (errno) ? errno : ENOSPC;
				WRITE_ERROR_EXIT;
			}

			status = LZ4F_freeCompressionContext(state->ctx);
			if (LZ4F_isError(status))
				pg_fatal("could not end compression: %s",
						 LZ4F_getErrorName(status));
		}
		else
		{
			status = LZ4F_freeDecompressionContext(state->dtx);
			if (LZ4F_isError(status))
				pg_fatal("could not end decompression: %s",
						 LZ4F_getErrorName(status));
			pg_free(state->overflowbuf);
		}

		pg_free(state->buffer);
	}

	pg_free(state);

	return fclose(fp) == 0;
}

static bool
LZ4Stream_open(const char *path, int fd, const char *mode,
			   CompressFileHandle *CFH)
{
	FILE	   *fp;
	LZ4State   *state = (LZ4State *) CFH->private_data;

	if (fd >= 0)
		fp = fdopen(fd, mode);
	else
		fp = fopen(path, mode);
	if (fp == NULL)
	{
		state->errcode = errno;
		return false;
	}

	state->fp = fp;

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
	state = pg_malloc0(sizeof(*state));
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
