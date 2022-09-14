/*-------------------------------------------------------------------------
 *
 * bbstreamer_zstd.c
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/bbstreamer_zstd.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>

#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "bbstreamer.h"
#include "common/logging.h"

#ifdef USE_ZSTD

typedef struct bbstreamer_zstd_frame
{
	bbstreamer	base;

	ZSTD_CCtx  *cctx;
	ZSTD_DCtx  *dctx;
	ZSTD_outBuffer zstd_outBuf;
} bbstreamer_zstd_frame;

static void bbstreamer_zstd_compressor_content(bbstreamer *streamer,
											   bbstreamer_member *member,
											   const char *data, int len,
											   bbstreamer_archive_context context);
static void bbstreamer_zstd_compressor_finalize(bbstreamer *streamer);
static void bbstreamer_zstd_compressor_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_zstd_compressor_ops = {
	.content = bbstreamer_zstd_compressor_content,
	.finalize = bbstreamer_zstd_compressor_finalize,
	.free = bbstreamer_zstd_compressor_free
};

static void bbstreamer_zstd_decompressor_content(bbstreamer *streamer,
												 bbstreamer_member *member,
												 const char *data, int len,
												 bbstreamer_archive_context context);
static void bbstreamer_zstd_decompressor_finalize(bbstreamer *streamer);
static void bbstreamer_zstd_decompressor_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_zstd_decompressor_ops = {
	.content = bbstreamer_zstd_decompressor_content,
	.finalize = bbstreamer_zstd_decompressor_finalize,
	.free = bbstreamer_zstd_decompressor_free
};
#endif

/*
 * Create a new base backup streamer that performs zstd compression of tar
 * blocks.
 */
bbstreamer *
bbstreamer_zstd_compressor_new(bbstreamer *next, pg_compress_specification *compress)
{
#ifdef USE_ZSTD
	bbstreamer_zstd_frame *streamer;
	size_t		ret;

	Assert(next != NULL);

	streamer = palloc0(sizeof(bbstreamer_zstd_frame));

	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_zstd_compressor_ops;

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

	/* Initialize the ZSTD output buffer. */
	streamer->zstd_outBuf.dst = streamer->base.bbs_buffer.data;
	streamer->zstd_outBuf.size = streamer->base.bbs_buffer.maxlen;
	streamer->zstd_outBuf.pos = 0;

	return &streamer->base;
#else
	pg_fatal("this build does not support zstd compression");
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
bbstreamer_zstd_compressor_content(bbstreamer *streamer,
								   bbstreamer_member *member,
								   const char *data, int len,
								   bbstreamer_archive_context context)
{
	bbstreamer_zstd_frame *mystreamer = (bbstreamer_zstd_frame *) streamer;
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
			bbstreamer_content(mystreamer->base.bbs_next, member,
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
bbstreamer_zstd_compressor_finalize(bbstreamer *streamer)
{
	bbstreamer_zstd_frame *mystreamer = (bbstreamer_zstd_frame *) streamer;
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
			bbstreamer_content(mystreamer->base.bbs_next, NULL,
							   mystreamer->zstd_outBuf.dst,
							   mystreamer->zstd_outBuf.pos,
							   BBSTREAMER_UNKNOWN);

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
		bbstreamer_content(mystreamer->base.bbs_next, NULL,
						   mystreamer->zstd_outBuf.dst,
						   mystreamer->zstd_outBuf.pos,
						   BBSTREAMER_UNKNOWN);

	bbstreamer_finalize(mystreamer->base.bbs_next);
}

/*
 * Free memory.
 */
static void
bbstreamer_zstd_compressor_free(bbstreamer *streamer)
{
	bbstreamer_zstd_frame *mystreamer = (bbstreamer_zstd_frame *) streamer;

	bbstreamer_free(streamer->bbs_next);
	ZSTD_freeCCtx(mystreamer->cctx);
	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}
#endif

/*
 * Create a new base backup streamer that performs decompression of zstd
 * compressed blocks.
 */
bbstreamer *
bbstreamer_zstd_decompressor_new(bbstreamer *next)
{
#ifdef USE_ZSTD
	bbstreamer_zstd_frame *streamer;

	Assert(next != NULL);

	streamer = palloc0(sizeof(bbstreamer_zstd_frame));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_zstd_decompressor_ops;

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
	pg_fatal("this build does not support zstd compression");
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
bbstreamer_zstd_decompressor_content(bbstreamer *streamer,
									 bbstreamer_member *member,
									 const char *data, int len,
									 bbstreamer_archive_context context)
{
	bbstreamer_zstd_frame *mystreamer = (bbstreamer_zstd_frame *) streamer;
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
			bbstreamer_content(mystreamer->base.bbs_next, member,
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
bbstreamer_zstd_decompressor_finalize(bbstreamer *streamer)
{
	bbstreamer_zstd_frame *mystreamer = (bbstreamer_zstd_frame *) streamer;

	/*
	 * End of the stream, if there is some pending data in output buffers then
	 * we must forward it to next streamer.
	 */
	if (mystreamer->zstd_outBuf.pos > 0)
		bbstreamer_content(mystreamer->base.bbs_next, NULL,
						   mystreamer->base.bbs_buffer.data,
						   mystreamer->base.bbs_buffer.maxlen,
						   BBSTREAMER_UNKNOWN);

	bbstreamer_finalize(mystreamer->base.bbs_next);
}

/*
 * Free memory.
 */
static void
bbstreamer_zstd_decompressor_free(bbstreamer *streamer)
{
	bbstreamer_zstd_frame *mystreamer = (bbstreamer_zstd_frame *) streamer;

	bbstreamer_free(streamer->bbs_next);
	ZSTD_freeDCtx(mystreamer->dctx);
	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}
#endif
