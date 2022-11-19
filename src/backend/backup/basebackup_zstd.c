/*-------------------------------------------------------------------------
 *
 * basebackup_zstd.c
 *	  Basebackup sink implementing zstd compression.
 *
 * Portions Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_zstd.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "backup/basebackup_sink.h"

#ifdef USE_ZSTD

typedef struct bbsink_zstd
{
	/* Common information for all types of sink. */
	bbsink		base;

	/* Compression options */
	pg_compress_specification *compress;

	ZSTD_CCtx  *cctx;
	ZSTD_outBuffer zstd_outBuf;
} bbsink_zstd;

static void bbsink_zstd_begin_backup(bbsink *sink);
static void bbsink_zstd_begin_archive(bbsink *sink, const char *archive_name);
static void bbsink_zstd_archive_contents(bbsink *sink, size_t avail_in);
static void bbsink_zstd_manifest_contents(bbsink *sink, size_t len);
static void bbsink_zstd_end_archive(bbsink *sink);
static void bbsink_zstd_cleanup(bbsink *sink);
static void bbsink_zstd_end_backup(bbsink *sink, XLogRecPtr endptr,
								   TimeLineID endtli);

static const bbsink_ops bbsink_zstd_ops = {
	.begin_backup = bbsink_zstd_begin_backup,
	.begin_archive = bbsink_zstd_begin_archive,
	.archive_contents = bbsink_zstd_archive_contents,
	.end_archive = bbsink_zstd_end_archive,
	.begin_manifest = bbsink_forward_begin_manifest,
	.manifest_contents = bbsink_zstd_manifest_contents,
	.end_manifest = bbsink_forward_end_manifest,
	.end_backup = bbsink_zstd_end_backup,
	.cleanup = bbsink_zstd_cleanup
};
#endif

/*
 * Create a new basebackup sink that performs zstd compression.
 */
bbsink *
bbsink_zstd_new(bbsink *next, pg_compress_specification *compress)
{
#ifndef USE_ZSTD
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("zstd compression is not supported by this build")));
	return NULL;				/* keep compiler quiet */
#else
	bbsink_zstd *sink;

	Assert(next != NULL);

	sink = palloc0(sizeof(bbsink_zstd));
	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_zstd_ops;
	sink->base.bbs_next = next;
	sink->compress = compress;

	return &sink->base;
#endif
}

#ifdef USE_ZSTD

/*
 * Begin backup.
 */
static void
bbsink_zstd_begin_backup(bbsink *sink)
{
	bbsink_zstd *mysink = (bbsink_zstd *) sink;
	size_t		output_buffer_bound;
	size_t		ret;
	pg_compress_specification *compress = mysink->compress;

	mysink->cctx = ZSTD_createCCtx();
	if (!mysink->cctx)
		elog(ERROR, "could not create zstd compression context");

	ret = ZSTD_CCtx_setParameter(mysink->cctx, ZSTD_c_compressionLevel,
								 compress->level);
	if (ZSTD_isError(ret))
		elog(ERROR, "could not set zstd compression level to %d: %s",
			 compress->level, ZSTD_getErrorName(ret));

	if ((compress->options & PG_COMPRESSION_OPTION_WORKERS) != 0)
	{
		/*
		 * On older versions of libzstd, this option does not exist, and
		 * trying to set it will fail. Similarly for newer versions if they
		 * are compiled without threading support.
		 */
		ret = ZSTD_CCtx_setParameter(mysink->cctx, ZSTD_c_nbWorkers,
									 compress->workers);
		if (ZSTD_isError(ret))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("could not set compression worker count to %d: %s",
						   compress->workers, ZSTD_getErrorName(ret)));
	}

	/*
	 * We need our own buffer, because we're going to pass different data to
	 * the next sink than what gets passed to us.
	 */
	mysink->base.bbs_buffer = palloc(mysink->base.bbs_buffer_length);

	/*
	 * Make sure that the next sink's bbs_buffer is big enough to accommodate
	 * the compressed input buffer.
	 */
	output_buffer_bound = ZSTD_compressBound(mysink->base.bbs_buffer_length);

	/*
	 * The buffer length is expected to be a multiple of BLCKSZ, so round up.
	 */
	output_buffer_bound = output_buffer_bound + BLCKSZ -
		(output_buffer_bound % BLCKSZ);

	bbsink_begin_backup(sink->bbs_next, sink->bbs_state, output_buffer_bound);
}

/*
 * Prepare to compress the next archive.
 */
static void
bbsink_zstd_begin_archive(bbsink *sink, const char *archive_name)
{
	bbsink_zstd *mysink = (bbsink_zstd *) sink;
	char	   *zstd_archive_name;

	/*
	 * At the start of each archive we reset the state to start a new
	 * compression operation. The parameters are sticky and they will stick
	 * around as we are resetting with option ZSTD_reset_session_only.
	 */
	ZSTD_CCtx_reset(mysink->cctx, ZSTD_reset_session_only);

	mysink->zstd_outBuf.dst = mysink->base.bbs_next->bbs_buffer;
	mysink->zstd_outBuf.size = mysink->base.bbs_next->bbs_buffer_length;
	mysink->zstd_outBuf.pos = 0;

	/* Add ".zst" to the archive name. */
	zstd_archive_name = psprintf("%s.zst", archive_name);
	Assert(sink->bbs_next != NULL);
	bbsink_begin_archive(sink->bbs_next, zstd_archive_name);
	pfree(zstd_archive_name);
}

/*
 * Compress the input data to the output buffer until we run out of input
 * data. Each time the output buffer falls below the compression bound for
 * the input buffer, invoke the archive_contents() method for the next sink.
 *
 * Note that since we're compressing the input, it may very commonly happen
 * that we consume all the input data without filling the output buffer. In
 * that case, the compressed representation of the current input data won't
 * actually be sent to the next bbsink until a later call to this function,
 * or perhaps even not until bbsink_zstd_end_archive() is invoked.
 */
static void
bbsink_zstd_archive_contents(bbsink *sink, size_t len)
{
	bbsink_zstd *mysink = (bbsink_zstd *) sink;
	ZSTD_inBuffer inBuf = {mysink->base.bbs_buffer, len, 0};

	while (inBuf.pos < inBuf.size)
	{
		size_t		yet_to_flush;
		size_t		max_needed = ZSTD_compressBound(inBuf.size - inBuf.pos);

		/*
		 * If the out buffer is not left with enough space, send the output
		 * buffer to the next sink, and reset it.
		 */
		if (mysink->zstd_outBuf.size - mysink->zstd_outBuf.pos < max_needed)
		{
			bbsink_archive_contents(mysink->base.bbs_next,
									mysink->zstd_outBuf.pos);
			mysink->zstd_outBuf.dst = mysink->base.bbs_next->bbs_buffer;
			mysink->zstd_outBuf.size =
				mysink->base.bbs_next->bbs_buffer_length;
			mysink->zstd_outBuf.pos = 0;
		}

		yet_to_flush = ZSTD_compressStream2(mysink->cctx, &mysink->zstd_outBuf,
											&inBuf, ZSTD_e_continue);

		if (ZSTD_isError(yet_to_flush))
			elog(ERROR,
				 "could not compress data: %s",
				 ZSTD_getErrorName(yet_to_flush));
	}
}

/*
 * There might be some data inside zstd's internal buffers; we need to get that
 * flushed out, also end the zstd frame and then get that forwarded to the
 * successor sink as archive content.
 *
 * Then we can end processing for this archive.
 */
static void
bbsink_zstd_end_archive(bbsink *sink)
{
	bbsink_zstd *mysink = (bbsink_zstd *) sink;
	size_t		yet_to_flush;

	do
	{
		ZSTD_inBuffer in = {NULL, 0, 0};
		size_t		max_needed = ZSTD_compressBound(0);

		/*
		 * If the out buffer is not left with enough space, send the output
		 * buffer to the next sink, and reset it.
		 */
		if (mysink->zstd_outBuf.size - mysink->zstd_outBuf.pos < max_needed)
		{
			bbsink_archive_contents(mysink->base.bbs_next,
									mysink->zstd_outBuf.pos);
			mysink->zstd_outBuf.dst = mysink->base.bbs_next->bbs_buffer;
			mysink->zstd_outBuf.size =
				mysink->base.bbs_next->bbs_buffer_length;
			mysink->zstd_outBuf.pos = 0;
		}

		yet_to_flush = ZSTD_compressStream2(mysink->cctx,
											&mysink->zstd_outBuf,
											&in, ZSTD_e_end);

		if (ZSTD_isError(yet_to_flush))
			elog(ERROR, "could not compress data: %s",
				 ZSTD_getErrorName(yet_to_flush));

	} while (yet_to_flush > 0);

	/* Make sure to pass any remaining bytes to the next sink. */
	if (mysink->zstd_outBuf.pos > 0)
		bbsink_archive_contents(mysink->base.bbs_next,
								mysink->zstd_outBuf.pos);

	/* Pass on the information that this archive has ended. */
	bbsink_forward_end_archive(sink);
}

/*
 * Free the resources and context.
 */
static void
bbsink_zstd_end_backup(bbsink *sink, XLogRecPtr endptr,
					   TimeLineID endtli)
{
	bbsink_zstd *mysink = (bbsink_zstd *) sink;

	/* Release the context. */
	if (mysink->cctx)
	{
		ZSTD_freeCCtx(mysink->cctx);
		mysink->cctx = NULL;
	}

	bbsink_forward_end_backup(sink, endptr, endtli);
}

/*
 * Manifest contents are not compressed, but we do need to copy them into
 * the successor sink's buffer, because we have our own.
 */
static void
bbsink_zstd_manifest_contents(bbsink *sink, size_t len)
{
	memcpy(sink->bbs_next->bbs_buffer, sink->bbs_buffer, len);
	bbsink_manifest_contents(sink->bbs_next, len);
}

/*
 * In case the backup fails, make sure we free any compression context that
 * got allocated, so that we don't leak memory.
 */
static void
bbsink_zstd_cleanup(bbsink *sink)
{
	bbsink_zstd *mysink = (bbsink_zstd *) sink;

	/* Release the context if not already released. */
	if (mysink->cctx)
	{
		ZSTD_freeCCtx(mysink->cctx);
		mysink->cctx = NULL;
	}
}

#endif
