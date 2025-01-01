/*-------------------------------------------------------------------------
 *
 * compress_zstd.c
 *	 Routines for archivers to write a Zstd compressed data stream.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_zstd.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "compress_zstd.h"
#include "pg_backup_utils.h"

#ifndef USE_ZSTD

void
InitCompressorZstd(CompressorState *cs, const pg_compress_specification compression_spec)
{
	pg_fatal("this build does not support compression with %s", "ZSTD");
}

void
InitCompressFileHandleZstd(CompressFileHandle *CFH, const pg_compress_specification compression_spec)
{
	pg_fatal("this build does not support compression with %s", "ZSTD");
}

#else

#include <zstd.h>

typedef struct ZstdCompressorState
{
	/* This is a normal file to which we read/write compressed data */
	FILE	   *fp;

	ZSTD_CStream *cstream;
	ZSTD_DStream *dstream;
	ZSTD_outBuffer output;
	ZSTD_inBuffer input;

	/* pointer to a static string like from strerror(), for Zstd_write() */
	const char *zstderror;
} ZstdCompressorState;

static ZSTD_CStream *_ZstdCStreamParams(pg_compress_specification compress);
static void EndCompressorZstd(ArchiveHandle *AH, CompressorState *cs);
static void WriteDataToArchiveZstd(ArchiveHandle *AH, CompressorState *cs,
								   const void *data, size_t dLen);
static void ReadDataFromArchiveZstd(ArchiveHandle *AH, CompressorState *cs);

static void
_Zstd_CCtx_setParam_or_die(ZSTD_CStream *cstream,
						   ZSTD_cParameter param, int value, char *paramname)
{
	size_t		res;

	res = ZSTD_CCtx_setParameter(cstream, param, value);
	if (ZSTD_isError(res))
		pg_fatal("could not set compression parameter \"%s\": %s",
				 paramname, ZSTD_getErrorName(res));
}

/* Return a compression stream with parameters set per argument */
static ZSTD_CStream *
_ZstdCStreamParams(pg_compress_specification compress)
{
	ZSTD_CStream *cstream;

	cstream = ZSTD_createCStream();
	if (cstream == NULL)
		pg_fatal("could not initialize compression library");

	_Zstd_CCtx_setParam_or_die(cstream, ZSTD_c_compressionLevel,
							   compress.level, "level");

	if (compress.options & PG_COMPRESSION_OPTION_LONG_DISTANCE)
		_Zstd_CCtx_setParam_or_die(cstream,
								   ZSTD_c_enableLongDistanceMatching,
								   compress.long_distance, "long");

	return cstream;
}

/* Helper function for WriteDataToArchiveZstd and EndCompressorZstd */
static void
_ZstdWriteCommon(ArchiveHandle *AH, CompressorState *cs, bool flush)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) cs->private_data;
	ZSTD_inBuffer *input = &zstdcs->input;
	ZSTD_outBuffer *output = &zstdcs->output;

	/* Loop while there's any input or until flushed */
	while (input->pos != input->size || flush)
	{
		size_t		res;

		output->pos = 0;
		res = ZSTD_compressStream2(zstdcs->cstream, output,
								   input, flush ? ZSTD_e_end : ZSTD_e_continue);

		if (ZSTD_isError(res))
			pg_fatal("could not compress data: %s", ZSTD_getErrorName(res));

		/*
		 * Extra paranoia: avoid zero-length chunks, since a zero length chunk
		 * is the EOF marker in the custom format. This should never happen
		 * but...
		 */
		if (output->pos > 0)
			cs->writeF(AH, output->dst, output->pos);

		if (res == 0)
			break;				/* End of frame or all input consumed */
	}
}

static void
EndCompressorZstd(ArchiveHandle *AH, CompressorState *cs)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) cs->private_data;

	if (cs->readF != NULL)
	{
		Assert(zstdcs->cstream == NULL);
		ZSTD_freeDStream(zstdcs->dstream);
		pg_free(unconstify(void *, zstdcs->input.src));
	}
	else if (cs->writeF != NULL)
	{
		Assert(zstdcs->dstream == NULL);
		_ZstdWriteCommon(AH, cs, true);
		ZSTD_freeCStream(zstdcs->cstream);
	}

	/* output buffer may be allocated in either mode */
	pg_free(zstdcs->output.dst);
	pg_free(zstdcs);
}

static void
WriteDataToArchiveZstd(ArchiveHandle *AH, CompressorState *cs,
					   const void *data, size_t dLen)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) cs->private_data;

	zstdcs->input.src = data;
	zstdcs->input.size = dLen;
	zstdcs->input.pos = 0;

	_ZstdWriteCommon(AH, cs, false);
}

static void
ReadDataFromArchiveZstd(ArchiveHandle *AH, CompressorState *cs)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) cs->private_data;
	ZSTD_outBuffer *output = &zstdcs->output;
	ZSTD_inBuffer *input = &zstdcs->input;
	size_t		input_allocated_size = ZSTD_DStreamInSize();
	size_t		res;

	for (;;)
	{
		size_t		cnt;

		/*
		 * Read compressed data.  Note that readF can resize the buffer; the
		 * new size is tracked and used for future loops.
		 */
		input->size = input_allocated_size;
		cnt = cs->readF(AH, (char **) unconstify(void **, &input->src), &input->size);

		/* ensure that readF didn't *shrink* the buffer */
		Assert(input->size >= input_allocated_size);
		input_allocated_size = input->size;
		input->size = cnt;
		input->pos = 0;

		if (cnt == 0)
			break;

		/* Now decompress */
		while (input->pos < input->size)
		{
			output->pos = 0;
			res = ZSTD_decompressStream(zstdcs->dstream, output, input);
			if (ZSTD_isError(res))
				pg_fatal("could not decompress data: %s", ZSTD_getErrorName(res));

			/*
			 * then write the decompressed data to the output handle
			 */
			((char *) output->dst)[output->pos] = '\0';
			ahwrite(output->dst, 1, output->pos, AH);

			if (res == 0)
				break;			/* End of frame */
		}
	}
}

/* Public routine that supports Zstd compressed data I/O */
void
InitCompressorZstd(CompressorState *cs,
				   const pg_compress_specification compression_spec)
{
	ZstdCompressorState *zstdcs;

	cs->readData = ReadDataFromArchiveZstd;
	cs->writeData = WriteDataToArchiveZstd;
	cs->end = EndCompressorZstd;

	cs->compression_spec = compression_spec;

	zstdcs = (ZstdCompressorState *) pg_malloc0(sizeof(*zstdcs));
	cs->private_data = zstdcs;

	/* We expect that exactly one of readF/writeF is specified */
	Assert((cs->readF == NULL) != (cs->writeF == NULL));

	if (cs->readF != NULL)
	{
		zstdcs->dstream = ZSTD_createDStream();
		if (zstdcs->dstream == NULL)
			pg_fatal("could not initialize compression library");

		zstdcs->input.size = ZSTD_DStreamInSize();
		zstdcs->input.src = pg_malloc(zstdcs->input.size);

		/*
		 * output.size is the buffer size we tell zstd it can output to.
		 * Allocate an additional byte such that ReadDataFromArchiveZstd() can
		 * call ahwrite() with a null-terminated string, which is an optimized
		 * case in ExecuteSqlCommandBuf().
		 */
		zstdcs->output.size = ZSTD_DStreamOutSize();
		zstdcs->output.dst = pg_malloc(zstdcs->output.size + 1);
	}
	else if (cs->writeF != NULL)
	{
		zstdcs->cstream = _ZstdCStreamParams(cs->compression_spec);

		zstdcs->output.size = ZSTD_CStreamOutSize();
		zstdcs->output.dst = pg_malloc(zstdcs->output.size);
		zstdcs->output.pos = 0;
	}
}

/*
 * Compressed stream API
 */

static bool
Zstd_read(void *ptr, size_t size, size_t *rdsize, CompressFileHandle *CFH)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) CFH->private_data;
	ZSTD_inBuffer *input = &zstdcs->input;
	ZSTD_outBuffer *output = &zstdcs->output;
	size_t		input_allocated_size = ZSTD_DStreamInSize();
	size_t		res,
				cnt;

	output->size = size;
	output->dst = ptr;
	output->pos = 0;

	for (;;)
	{
		Assert(input->pos <= input->size);
		Assert(input->size <= input_allocated_size);

		/*
		 * If the input is completely consumed, start back at the beginning
		 */
		if (input->pos == input->size)
		{
			/* input->size is size produced by "fread" */
			input->size = 0;
			/* input->pos is position consumed by decompress */
			input->pos = 0;
		}

		/* read compressed data if we must produce more input */
		if (input->pos == input->size)
		{
			cnt = fread(unconstify(void *, input->src), 1, input_allocated_size, zstdcs->fp);
			input->size = cnt;

			Assert(cnt <= input_allocated_size);

			/* If we have no more input to consume, we're done */
			if (cnt == 0)
				break;
		}

		while (input->pos < input->size)
		{
			/* now decompress */
			res = ZSTD_decompressStream(zstdcs->dstream, output, input);

			if (ZSTD_isError(res))
				pg_fatal("could not decompress data: %s", ZSTD_getErrorName(res));

			if (output->pos == output->size)
				break;			/* No more room for output */

			if (res == 0)
				break;			/* End of frame */
		}

		if (output->pos == output->size)
			break;				/* We read all the data that fits */
	}

	if (rdsize != NULL)
		*rdsize = output->pos;

	return true;
}

static bool
Zstd_write(const void *ptr, size_t size, CompressFileHandle *CFH)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) CFH->private_data;
	ZSTD_inBuffer *input = &zstdcs->input;
	ZSTD_outBuffer *output = &zstdcs->output;
	size_t		res,
				cnt;

	input->src = ptr;
	input->size = size;
	input->pos = 0;

	/* Consume all input, to be flushed later */
	while (input->pos != input->size)
	{
		output->pos = 0;
		res = ZSTD_compressStream2(zstdcs->cstream, output, input, ZSTD_e_continue);
		if (ZSTD_isError(res))
		{
			zstdcs->zstderror = ZSTD_getErrorName(res);
			return false;
		}

		cnt = fwrite(output->dst, 1, output->pos, zstdcs->fp);
		if (cnt != output->pos)
		{
			zstdcs->zstderror = strerror(errno);
			return false;
		}
	}

	return size;
}

static int
Zstd_getc(CompressFileHandle *CFH)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) CFH->private_data;
	int			ret;

	if (CFH->read_func(&ret, 1, NULL, CFH) != 1)
	{
		if (feof(zstdcs->fp))
			pg_fatal("could not read from input file: end of file");
		else
			pg_fatal("could not read from input file: %m");
	}
	return ret;
}

static char *
Zstd_gets(char *buf, int len, CompressFileHandle *CFH)
{
	int			i;

	Assert(len > 0);

	/*
	 * Read one byte at a time until newline or EOF. This is only used to read
	 * the list of LOs, and the I/O is buffered anyway.
	 */
	for (i = 0; i < len - 1; ++i)
	{
		size_t		readsz;

		if (!CFH->read_func(&buf[i], 1, &readsz, CFH))
			break;
		if (readsz != 1)
			break;
		if (buf[i] == '\n')
		{
			++i;
			break;
		}
	}
	buf[i] = '\0';
	return i > 0 ? buf : NULL;
}

static bool
Zstd_close(CompressFileHandle *CFH)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) CFH->private_data;

	if (zstdcs->cstream)
	{
		size_t		res,
					cnt;
		ZSTD_inBuffer *input = &zstdcs->input;
		ZSTD_outBuffer *output = &zstdcs->output;

		/* Loop until the compression buffers are fully consumed */
		for (;;)
		{
			output->pos = 0;
			res = ZSTD_compressStream2(zstdcs->cstream, output, input, ZSTD_e_end);
			if (ZSTD_isError(res))
			{
				zstdcs->zstderror = ZSTD_getErrorName(res);
				return false;
			}

			cnt = fwrite(output->dst, 1, output->pos, zstdcs->fp);
			if (cnt != output->pos)
			{
				zstdcs->zstderror = strerror(errno);
				return false;
			}

			if (res == 0)
				break;			/* End of frame */
		}

		ZSTD_freeCStream(zstdcs->cstream);
		pg_free(zstdcs->output.dst);
	}

	if (zstdcs->dstream)
	{
		ZSTD_freeDStream(zstdcs->dstream);
		pg_free(unconstify(void *, zstdcs->input.src));
	}

	if (fclose(zstdcs->fp) != 0)
		return false;

	pg_free(zstdcs);
	return true;
}

static bool
Zstd_eof(CompressFileHandle *CFH)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) CFH->private_data;

	return feof(zstdcs->fp);
}

static bool
Zstd_open(const char *path, int fd, const char *mode,
		  CompressFileHandle *CFH)
{
	FILE	   *fp;
	ZstdCompressorState *zstdcs;

	if (fd >= 0)
		fp = fdopen(fd, mode);
	else
		fp = fopen(path, mode);

	if (fp == NULL)
		return false;

	zstdcs = (ZstdCompressorState *) pg_malloc0(sizeof(*zstdcs));
	CFH->private_data = zstdcs;
	zstdcs->fp = fp;

	if (mode[0] == 'r')
	{
		zstdcs->input.src = pg_malloc0(ZSTD_DStreamInSize());
		zstdcs->dstream = ZSTD_createDStream();
		if (zstdcs->dstream == NULL)
			pg_fatal("could not initialize compression library");
	}
	else if (mode[0] == 'w' || mode[0] == 'a')
	{
		zstdcs->output.size = ZSTD_CStreamOutSize();
		zstdcs->output.dst = pg_malloc0(zstdcs->output.size);
		zstdcs->cstream = _ZstdCStreamParams(CFH->compression_spec);
		if (zstdcs->cstream == NULL)
			pg_fatal("could not initialize compression library");
	}
	else
		pg_fatal("unhandled mode \"%s\"", mode);

	return true;
}

static bool
Zstd_open_write(const char *path, const char *mode, CompressFileHandle *CFH)
{
	char		fname[MAXPGPATH];

	sprintf(fname, "%s.zst", path);
	return CFH->open_func(fname, -1, mode, CFH);
}

static const char *
Zstd_get_error(CompressFileHandle *CFH)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) CFH->private_data;

	return zstdcs->zstderror;
}

void
InitCompressFileHandleZstd(CompressFileHandle *CFH,
						   const pg_compress_specification compression_spec)
{
	CFH->open_func = Zstd_open;
	CFH->open_write_func = Zstd_open_write;
	CFH->read_func = Zstd_read;
	CFH->write_func = Zstd_write;
	CFH->gets_func = Zstd_gets;
	CFH->getc_func = Zstd_getc;
	CFH->close_func = Zstd_close;
	CFH->eof_func = Zstd_eof;
	CFH->get_error_func = Zstd_get_error;

	CFH->compression_spec = compression_spec;

	CFH->private_data = NULL;
}

#endif							/* USE_ZSTD */
