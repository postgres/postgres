/*-------------------------------------------------------------------------
 *
 * astreamer_zstd.c
 *
 * Archive streamers that deal with data compressed using zstd.
 * astreamer_zstd_compressor applies lz4 compression to the input stream,
 * and astreamer_zstd_decompressor does the reverse.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/fe_utils/astreamer_zstd.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>

#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "common/logging.h"
#include "fe_utils/astreamer.h"

#ifdef USE_ZSTD

typedef struct astreamer_zstd_frame
{
	astreamer	base;

	ZSTD_CCtx  *cctx;
	ZSTD_DCtx  *dctx;
	ZSTD_outBuffer zstd_outBuf;
} astreamer_zstd_frame;

static void astreamer_zstd_compressor_content(astreamer *streamer,
											  astreamer_member *member,
											  const char *data, int len,
											  astreamer_archive_context context);
static void astreamer_zstd_compressor_finalize(astreamer *streamer);
static void astreamer_zstd_compressor_free(astreamer *streamer);

static const astreamer_ops astreamer_zstd_compressor_ops = {
	.content = astreamer_zstd_compressor_content,
	.finalize = astreamer_zstd_compressor_finalize,
	.free = astreamer_zstd_compressor_free
};

static void astreamer_zstd_decompressor_content(astreamer *streamer,
												astreamer_member *member,
												const char *data, int len,
												astreamer_archive_context context);
static void astreamer_zstd_decompressor_finalize(astreamer *streamer);
static void astreamer_zstd_decompressor_free(astreamer *streamer);

static const astreamer_ops astreamer_zstd_decompressor_ops = {
	.content = astreamer_zstd_decompressor_content,
	.finalize = astreamer_zstd_decompressor_finalize,
	.free = astreamer_zstd_decompressor_free
};
#endif

/*
 * Create a new base backup streamer that performs zstd compression of tar
 * blocks.
 */
astreamer *
astreamer_zstd_compressor_new(astreamer *next, pg_compress_specification *compress)
{
#ifdef USE_ZSTD
	astreamer_zstd_frame *streamer;
	size_t		ret;

	Assert(next != NULL);

	streamer = palloc0(sizeof(astreamer_zstd_frame));

	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_zstd_compressor_ops;

	streamer->base.bbs_next = next;
	initStringInfo(&streamer->base.bbs_buffer);
	enlargeStringInfo(&streamer->base.bbs_buffer, ZSTD_DStreamOutSize());

	streamer->cctx = ZSTD_createCCtx();
	if (!streamer->cctx)
		pg_fatal("could not create zstd compression context");

	/* Set compression level */
	ret = ZSTD_CCtx_setParameter(streamer->cctx, ZSTD_c_compressionLevel,
								 compress->level);
	if (ZSTD_isError(ret))
		pg_fatal("could not set zstd compression level to %d: %s",
				 compress->level, ZSTD_getErrorName(ret));

	/* Set # of workers, if specified */
	if ((compress->options & PG_COMPRESSION_OPTION_WORKERS) != 0)
	{
		/*
		 * On older versions of libzstd, this option does not exist, and
		 * trying to set it will fail. Similarly for newer versions if they
		 * are compiled without threading support.
		 */
		ret = ZSTD_CCtx_setParameter(streamer->cctx, ZSTD_c_nbWorkers,
									 compress->workers);
		if (ZSTD_isError(ret))
			pg_fatal("could not set compression worker count to %d: %s",
					 compress->workers, ZSTD_getErrorName(ret));
	}

	if ((compress->options & PG_COMPRESSION_OPTION_LONG_DISTANCE) != 0)
	{
		ret = ZSTD_CCtx_setParameter(streamer->cctx,
									 ZSTD_c_enableLongDistanceMatching,
									 compress->long_distance);
		if (ZSTD_isError(ret))
		{
			pg_log_error("could not enable long-distance mode: %s",
						 ZSTD_getErrorName(ret));
			exit(1);
		}
	}

	/* Initialize the ZSTD output buffer. */
	streamer->zstd_outBuf.dst = streamer->base.bbs_buffer.data;
	streamer->zstd_outBuf.size = streamer->base.bbs_buffer.maxlen;
	streamer->zstd_outBuf.pos = 0;

	return &streamer->base;
#else
	pg_fatal("this build does not support compression with %s", "ZSTD");
	return NULL;				/* keep compiler quiet */
#endif
}

#ifdef USE_ZSTD
/*
 * Compress the input data to output buffer.
 *
 * Find out the compression bound based on input data length for each
 * invocation to make sure that output buffer has enough capacity to
 * accommodate the compressed data. In case if the output buffer
 * capacity falls short of compression bound then forward the content
 * of output buffer to next streamer and empty the buffer.
 */
static void
astreamer_zstd_compressor_content(astreamer *streamer,
								  astreamer_member *member,
								  const char *data, int len,
								  astreamer_archive_context context)
{
	astreamer_zstd_frame *mystreamer = (astreamer_zstd_frame *) streamer;
	ZSTD_inBuffer inBuf = {data, len, 0};

	while (inBuf.pos < inBuf.size)
	{
		size_t		yet_to_flush;
		size_t		max_needed = ZSTD_compressBound(inBuf.size - inBuf.pos);

		/*
		 * If the output buffer is not left with enough space, send the
		 * compressed bytes to the next streamer, and empty the buffer.
		 */
		if (mystreamer->zstd_outBuf.size - mystreamer->zstd_outBuf.pos <
			max_needed)
		{
			astreamer_content(mystreamer->base.bbs_next, member,
							  mystreamer->zstd_outBuf.dst,
							  mystreamer->zstd_outBuf.pos,
							  context);

			/* Reset the ZSTD output buffer. */
			mystreamer->zstd_outBuf.dst = mystreamer->base.bbs_buffer.data;
			mystreamer->zstd_outBuf.size = mystreamer->base.bbs_buffer.maxlen;
			mystreamer->zstd_outBuf.pos = 0;
		}

		yet_to_flush =
			ZSTD_compressStream2(mystreamer->cctx, &mystreamer->zstd_outBuf,
								 &inBuf, ZSTD_e_continue);

		if (ZSTD_isError(yet_to_flush))
			pg_log_error("could not compress data: %s",
						 ZSTD_getErrorName(yet_to_flush));
	}
}

/*
 * End-of-stream processing.
 */
static void
astreamer_zstd_compressor_finalize(astreamer *streamer)
{
	astreamer_zstd_frame *mystreamer = (astreamer_zstd_frame *) streamer;
	size_t		yet_to_flush;

	do
	{
		ZSTD_inBuffer in = {NULL, 0, 0};
		size_t		max_needed = ZSTD_compressBound(0);

		/*
		 * If the output buffer is not left with enough space, send the
		 * compressed bytes to the next streamer, and empty the buffer.
		 */
		if (mystreamer->zstd_outBuf.size - mystreamer->zstd_outBuf.pos <
			max_needed)
		{
			astreamer_content(mystreamer->base.bbs_next, NULL,
							  mystreamer->zstd_outBuf.dst,
							  mystreamer->zstd_outBuf.pos,
							  ASTREAMER_UNKNOWN);

			/* Reset the ZSTD output buffer. */
			mystreamer->zstd_outBuf.dst = mystreamer->base.bbs_buffer.data;
			mystreamer->zstd_outBuf.size = mystreamer->base.bbs_buffer.maxlen;
			mystreamer->zstd_outBuf.pos = 0;
		}

		yet_to_flush = ZSTD_compressStream2(mystreamer->cctx,
											&mystreamer->zstd_outBuf,
											&in, ZSTD_e_end);

		if (ZSTD_isError(yet_to_flush))
			pg_log_error("could not compress data: %s",
						 ZSTD_getErrorName(yet_to_flush));

	} while (yet_to_flush > 0);

	/* Make sure to pass any remaining bytes to the next streamer. */
	if (mystreamer->zstd_outBuf.pos > 0)
		astreamer_content(mystreamer->base.bbs_next, NULL,
						  mystreamer->zstd_outBuf.dst,
						  mystreamer->zstd_outBuf.pos,
						  ASTREAMER_UNKNOWN);

	astreamer_finalize(mystreamer->base.bbs_next);
}

/*
 * Free memory.
 */
static void
astreamer_zstd_compressor_free(astreamer *streamer)
{
	astreamer_zstd_frame *mystreamer = (astreamer_zstd_frame *) streamer;

	astreamer_free(streamer->bbs_next);
	ZSTD_freeCCtx(mystreamer->cctx);
	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}
#endif

/*
 * Create a new base backup streamer that performs decompression of zstd
 * compressed blocks.
 */
astreamer *
astreamer_zstd_decompressor_new(astreamer *next)
{
#ifdef USE_ZSTD
	astreamer_zstd_frame *streamer;

	Assert(next != NULL);

	streamer = palloc0(sizeof(astreamer_zstd_frame));
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_zstd_decompressor_ops;

	streamer->base.bbs_next = next;
	initStringInfo(&streamer->base.bbs_buffer);
	enlargeStringInfo(&streamer->base.bbs_buffer, ZSTD_DStreamOutSize());

	streamer->dctx = ZSTD_createDCtx();
	if (!streamer->dctx)
		pg_fatal("could not create zstd decompression context");

	/* Initialize the ZSTD output buffer. */
	streamer->zstd_outBuf.dst = streamer->base.bbs_buffer.data;
	streamer->zstd_outBuf.size = streamer->base.bbs_buffer.maxlen;
	streamer->zstd_outBuf.pos = 0;

	return &streamer->base;
#else
	pg_fatal("this build does not support compression with %s", "ZSTD");
	return NULL;				/* keep compiler quiet */
#endif
}

#ifdef USE_ZSTD
/*
 * Decompress the input data to output buffer until we run out of input
 * data. Each time the output buffer is full, pass on the decompressed data
 * to the next streamer.
 */
static void
astreamer_zstd_decompressor_content(astreamer *streamer,
									astreamer_member *member,
									const char *data, int len,
									astreamer_archive_context context)
{
	astreamer_zstd_frame *mystreamer = (astreamer_zstd_frame *) streamer;
	ZSTD_inBuffer inBuf = {data, len, 0};

	while (inBuf.pos < inBuf.size)
	{
		size_t		ret;

		/*
		 * If output buffer is full then forward the content to next streamer
		 * and update the output buffer.
		 */
		if (mystreamer->zstd_outBuf.pos >= mystreamer->zstd_outBuf.size)
		{
			astreamer_content(mystreamer->base.bbs_next, member,
							  mystreamer->zstd_outBuf.dst,
							  mystreamer->zstd_outBuf.pos,
							  context);

			/* Reset the ZSTD output buffer. */
			mystreamer->zstd_outBuf.dst = mystreamer->base.bbs_buffer.data;
			mystreamer->zstd_outBuf.size = mystreamer->base.bbs_buffer.maxlen;
			mystreamer->zstd_outBuf.pos = 0;
		}

		ret = ZSTD_decompressStream(mystreamer->dctx,
									&mystreamer->zstd_outBuf, &inBuf);

		if (ZSTD_isError(ret))
			pg_log_error("could not decompress data: %s",
						 ZSTD_getErrorName(ret));
	}
}

/*
 * End-of-stream processing.
 */
static void
astreamer_zstd_decompressor_finalize(astreamer *streamer)
{
	astreamer_zstd_frame *mystreamer = (astreamer_zstd_frame *) streamer;

	/*
	 * End of the stream, if there is some pending data in output buffers then
	 * we must forward it to next streamer.
	 */
	if (mystreamer->zstd_outBuf.pos > 0)
		astreamer_content(mystreamer->base.bbs_next, NULL,
						  mystreamer->base.bbs_buffer.data,
						  mystreamer->base.bbs_buffer.maxlen,
						  ASTREAMER_UNKNOWN);

	astreamer_finalize(mystreamer->base.bbs_next);
}

/*
 * Free memory.
 */
static void
astreamer_zstd_decompressor_free(astreamer *streamer)
{
	astreamer_zstd_frame *mystreamer = (astreamer_zstd_frame *) streamer;

	astreamer_free(streamer->bbs_next);
	ZSTD_freeDCtx(mystreamer->dctx);
	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}
#endif
