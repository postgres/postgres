/*-------------------------------------------------------------------------
 *
 * basebackup_lz4.c
 *	  Basebackup sink implementing lz4 compression.
 *
 * Portions Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_lz4.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_LZ4
#include <lz4frame.h>
#endif

#include "backup/basebackup_sink.h"

#ifdef USE_LZ4

typedef struct bbsink_lz4
{
	/* Common information for all types of sink. */
	bbsink		base;

	/* Compression level. */
	int			compresslevel;

	LZ4F_compressionContext_t ctx;
	LZ4F_preferences_t prefs;

	/* Number of bytes staged in output buffer. */
	size_t		bytes_written;
} bbsink_lz4;

static void bbsink_lz4_begin_backup(bbsink *sink);
static void bbsink_lz4_begin_archive(bbsink *sink, const char *archive_name);
static void bbsink_lz4_archive_contents(bbsink *sink, size_t avail_in);
static void bbsink_lz4_manifest_contents(bbsink *sink, size_t len);
static void bbsink_lz4_end_archive(bbsink *sink);
static void bbsink_lz4_cleanup(bbsink *sink);

static const bbsink_ops bbsink_lz4_ops = {
	.begin_backup = bbsink_lz4_begin_backup,
	.begin_archive = bbsink_lz4_begin_archive,
	.archive_contents = bbsink_lz4_archive_contents,
	.end_archive = bbsink_lz4_end_archive,
	.begin_manifest = bbsink_forward_begin_manifest,
	.manifest_contents = bbsink_lz4_manifest_contents,
	.end_manifest = bbsink_forward_end_manifest,
	.end_backup = bbsink_forward_end_backup,
	.cleanup = bbsink_lz4_cleanup
};
#endif

/*
 * Create a new basebackup sink that performs lz4 compression.
 */
bbsink *
bbsink_lz4_new(bbsink *next, pg_compress_specification *compress)
{
#ifndef USE_LZ4
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("lz4 compression is not supported by this build")));
	return NULL;				/* keep compiler quiet */
#else
	bbsink_lz4 *sink;
	int			compresslevel;

	Assert(next != NULL);

	compresslevel = compress->level;
	Assert(compresslevel >= 0 && compresslevel <= 12);

	sink = palloc0(sizeof(bbsink_lz4));
	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_lz4_ops;
	sink->base.bbs_next = next;
	sink->compresslevel = compresslevel;

	return &sink->base;
#endif
}

#ifdef USE_LZ4

/*
 * Begin backup.
 */
static void
bbsink_lz4_begin_backup(bbsink *sink)
{
	bbsink_lz4 *mysink = (bbsink_lz4 *) sink;
	size_t		output_buffer_bound;
	LZ4F_preferences_t *prefs = &mysink->prefs;

	/* Initialize compressor object. */
	memset(prefs, 0, sizeof(LZ4F_preferences_t));
	prefs->frameInfo.blockSizeID = LZ4F_max256KB;
	prefs->compressionLevel = mysink->compresslevel;

	/*
	 * We need our own buffer, because we're going to pass different data to
	 * the next sink than what gets passed to us.
	 */
	mysink->base.bbs_buffer = palloc(mysink->base.bbs_buffer_length);

	/*
	 * Since LZ4F_compressUpdate() requires the output buffer of size equal or
	 * greater than that of LZ4F_compressBound(), make sure we have the next
	 * sink's bbs_buffer of length that can accommodate the compressed input
	 * buffer.
	 */
	output_buffer_bound = LZ4F_compressBound(mysink->base.bbs_buffer_length,
											 &mysink->prefs);

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
bbsink_lz4_begin_archive(bbsink *sink, const char *archive_name)
{
	bbsink_lz4 *mysink = (bbsink_lz4 *) sink;
	char	   *lz4_archive_name;
	LZ4F_errorCode_t ctxError;
	size_t		headerSize;

	ctxError = LZ4F_createCompressionContext(&mysink->ctx, LZ4F_VERSION);
	if (LZ4F_isError(ctxError))
		elog(ERROR, "could not create lz4 compression context: %s",
			 LZ4F_getErrorName(ctxError));

	/* First of all write the frame header to destination buffer. */
	headerSize = LZ4F_compressBegin(mysink->ctx,
									mysink->base.bbs_next->bbs_buffer,
									mysink->base.bbs_next->bbs_buffer_length,
									&mysink->prefs);

	if (LZ4F_isError(headerSize))
		elog(ERROR, "could not write lz4 header: %s",
			 LZ4F_getErrorName(headerSize));

	/*
	 * We need to write the compressed data after the header in the output
	 * buffer. So, make sure to update the notion of bytes written to output
	 * buffer.
	 */
	mysink->bytes_written += headerSize;

	/* Add ".lz4" to the archive name. */
	lz4_archive_name = psprintf("%s.lz4", archive_name);
	Assert(sink->bbs_next != NULL);
	bbsink_begin_archive(sink->bbs_next, lz4_archive_name);
	pfree(lz4_archive_name);
}

/*
 * Compress the input data to the output buffer until we run out of input
 * data. Each time the output buffer falls below the compression bound for
 * the input buffer, invoke the archive_contents() method for then next sink.
 *
 * Note that since we're compressing the input, it may very commonly happen
 * that we consume all the input data without filling the output buffer. In
 * that case, the compressed representation of the current input data won't
 * actually be sent to the next bbsink until a later call to this function,
 * or perhaps even not until bbsink_lz4_end_archive() is invoked.
 */
static void
bbsink_lz4_archive_contents(bbsink *sink, size_t avail_in)
{
	bbsink_lz4 *mysink = (bbsink_lz4 *) sink;
	size_t		compressedSize;
	size_t		avail_in_bound;

	avail_in_bound = LZ4F_compressBound(avail_in, &mysink->prefs);

	/*
	 * If the number of available bytes has fallen below the value computed by
	 * LZ4F_compressBound(), ask the next sink to process the data so that we
	 * can empty the buffer.
	 */
	if ((mysink->base.bbs_next->bbs_buffer_length - mysink->bytes_written) <
		avail_in_bound)
	{
		bbsink_archive_contents(sink->bbs_next, mysink->bytes_written);
		mysink->bytes_written = 0;
	}

	/*
	 * Compress the input buffer and write it into the output buffer.
	 */
	compressedSize = LZ4F_compressUpdate(mysink->ctx,
										 mysink->base.bbs_next->bbs_buffer + mysink->bytes_written,
										 mysink->base.bbs_next->bbs_buffer_length - mysink->bytes_written,
										 (uint8 *) mysink->base.bbs_buffer,
										 avail_in,
										 NULL);

	if (LZ4F_isError(compressedSize))
		elog(ERROR, "could not compress data: %s",
			 LZ4F_getErrorName(compressedSize));

	/*
	 * Update our notion of how many bytes we've written into output buffer.
	 */
	mysink->bytes_written += compressedSize;
}

/*
 * There might be some data inside lz4's internal buffers; we need to get
 * that flushed out and also finalize the lz4 frame and then get that forwarded
 * to the successor sink as archive content.
 *
 * Then we can end processing for this archive.
 */
static void
bbsink_lz4_end_archive(bbsink *sink)
{
	bbsink_lz4 *mysink = (bbsink_lz4 *) sink;
	size_t		compressedSize;
	size_t		lz4_footer_bound;

	lz4_footer_bound = LZ4F_compressBound(0, &mysink->prefs);

	Assert(mysink->base.bbs_next->bbs_buffer_length >= lz4_footer_bound);

	if ((mysink->base.bbs_next->bbs_buffer_length - mysink->bytes_written) <
		lz4_footer_bound)
	{
		bbsink_archive_contents(sink->bbs_next, mysink->bytes_written);
		mysink->bytes_written = 0;
	}

	compressedSize = LZ4F_compressEnd(mysink->ctx,
									  mysink->base.bbs_next->bbs_buffer + mysink->bytes_written,
									  mysink->base.bbs_next->bbs_buffer_length - mysink->bytes_written,
									  NULL);

	if (LZ4F_isError(compressedSize))
		elog(ERROR, "could not end lz4 compression: %s",
			 LZ4F_getErrorName(compressedSize));

	/* Update our notion of how many bytes we've written. */
	mysink->bytes_written += compressedSize;

	/* Send whatever accumulated output bytes we have. */
	bbsink_archive_contents(sink->bbs_next, mysink->bytes_written);
	mysink->bytes_written = 0;

	/* Release the resources. */
	LZ4F_freeCompressionContext(mysink->ctx);
	mysink->ctx = NULL;

	/* Pass on the information that this archive has ended. */
	bbsink_forward_end_archive(sink);
}

/*
 * Manifest contents are not compressed, but we do need to copy them into
 * the successor sink's buffer, because we have our own.
 */
static void
bbsink_lz4_manifest_contents(bbsink *sink, size_t len)
{
	memcpy(sink->bbs_next->bbs_buffer, sink->bbs_buffer, len);
	bbsink_manifest_contents(sink->bbs_next, len);
}

/*
 * In case the backup fails, make sure we free the compression context by
 * calling LZ4F_freeCompressionContext() if needed to avoid memory leak.
 */
static void
bbsink_lz4_cleanup(bbsink *sink)
{
	bbsink_lz4 *mysink = (bbsink_lz4 *) sink;

	if (mysink->ctx)
	{
		LZ4F_freeCompressionContext(mysink->ctx);
		mysink->ctx = NULL;
	}
}

#endif
