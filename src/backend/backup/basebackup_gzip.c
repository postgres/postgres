/*-------------------------------------------------------------------------
 *
 * basebackup_gzip.c
 *	  Basebackup sink implementing gzip compression.
 *
 * Portions Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_gzip.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "backup/basebackup_sink.h"

#ifdef HAVE_LIBZ
typedef struct bbsink_gzip
{
	/* Common information for all types of sink. */
	bbsink		base;

	/* Compression level. */
	int			compresslevel;

	/* Compressed data stream. */
	z_stream	zstream;

	/* Number of bytes staged in output buffer. */
	size_t		bytes_written;
} bbsink_gzip;

static void bbsink_gzip_begin_backup(bbsink *sink);
static void bbsink_gzip_begin_archive(bbsink *sink, const char *archive_name);
static void bbsink_gzip_archive_contents(bbsink *sink, size_t len);
static void bbsink_gzip_manifest_contents(bbsink *sink, size_t len);
static void bbsink_gzip_end_archive(bbsink *sink);
static void *gzip_palloc(void *opaque, unsigned items, unsigned size);
static void gzip_pfree(void *opaque, void *address);

static const bbsink_ops bbsink_gzip_ops = {
	.begin_backup = bbsink_gzip_begin_backup,
	.begin_archive = bbsink_gzip_begin_archive,
	.archive_contents = bbsink_gzip_archive_contents,
	.end_archive = bbsink_gzip_end_archive,
	.begin_manifest = bbsink_forward_begin_manifest,
	.manifest_contents = bbsink_gzip_manifest_contents,
	.end_manifest = bbsink_forward_end_manifest,
	.end_backup = bbsink_forward_end_backup,
	.cleanup = bbsink_forward_cleanup
};
#endif

/*
 * Create a new basebackup sink that performs gzip compression.
 */
bbsink *
bbsink_gzip_new(bbsink *next, pg_compress_specification *compress)
{
#ifndef HAVE_LIBZ
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("gzip compression is not supported by this build")));
	return NULL;				/* keep compiler quiet */
#else
	bbsink_gzip *sink;
	int			compresslevel;

	Assert(next != NULL);

	compresslevel = compress->level;
	Assert((compresslevel >= 1 && compresslevel <= 9) ||
		   compresslevel == Z_DEFAULT_COMPRESSION);

	sink = palloc0(sizeof(bbsink_gzip));
	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_gzip_ops;
	sink->base.bbs_next = next;
	sink->compresslevel = compresslevel;

	return &sink->base;
#endif
}

#ifdef HAVE_LIBZ

/*
 * Begin backup.
 */
static void
bbsink_gzip_begin_backup(bbsink *sink)
{
	/*
	 * We need our own buffer, because we're going to pass different data to
	 * the next sink than what gets passed to us.
	 */
	sink->bbs_buffer = palloc(sink->bbs_buffer_length);

	/*
	 * Since deflate() doesn't require the output buffer to be of any
	 * particular size, we can just make it the same size as the input buffer.
	 */
	bbsink_begin_backup(sink->bbs_next, sink->bbs_state,
						sink->bbs_buffer_length);
}

/*
 * Prepare to compress the next archive.
 */
static void
bbsink_gzip_begin_archive(bbsink *sink, const char *archive_name)
{
	bbsink_gzip *mysink = (bbsink_gzip *) sink;
	char	   *gz_archive_name;
	z_stream   *zs = &mysink->zstream;

	/* Initialize compressor object. */
	memset(zs, 0, sizeof(z_stream));
	zs->zalloc = gzip_palloc;
	zs->zfree = gzip_pfree;
	zs->next_out = (uint8 *) sink->bbs_next->bbs_buffer;
	zs->avail_out = sink->bbs_next->bbs_buffer_length;

	/*
	 * We need to use deflateInit2() rather than deflateInit() here so that we
	 * can request a gzip header rather than a zlib header. Otherwise, we want
	 * to supply the same values that would have been used by default if we
	 * had just called deflateInit().
	 *
	 * Per the documentation for deflateInit2, the third argument must be
	 * Z_DEFLATED; the fourth argument is the number of "window bits", by
	 * default 15, but adding 16 gets you a gzip header rather than a zlib
	 * header; the fifth argument controls memory usage, and 8 is the default;
	 * and likewise Z_DEFAULT_STRATEGY is the default for the sixth argument.
	 */
	if (deflateInit2(zs, mysink->compresslevel, Z_DEFLATED, 15 + 16, 8,
					 Z_DEFAULT_STRATEGY) != Z_OK)
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not initialize compression library"));

	/*
	 * Add ".gz" to the archive name. Note that the pg_basebackup -z produces
	 * archives named ".tar.gz" rather than ".tgz", so we match that here.
	 */
	gz_archive_name = psprintf("%s.gz", archive_name);
	Assert(sink->bbs_next != NULL);
	bbsink_begin_archive(sink->bbs_next, gz_archive_name);
	pfree(gz_archive_name);
}

/*
 * Compress the input data to the output buffer until we run out of input
 * data. Each time the output buffer fills up, invoke the archive_contents()
 * method for then next sink.
 *
 * Note that since we're compressing the input, it may very commonly happen
 * that we consume all the input data without filling the output buffer. In
 * that case, the compressed representation of the current input data won't
 * actually be sent to the next bbsink until a later call to this function,
 * or perhaps even not until bbsink_gzip_end_archive() is invoked.
 */
static void
bbsink_gzip_archive_contents(bbsink *sink, size_t len)
{
	bbsink_gzip *mysink = (bbsink_gzip *) sink;
	z_stream   *zs = &mysink->zstream;

	/* Compress data from input buffer. */
	zs->next_in = (uint8 *) mysink->base.bbs_buffer;
	zs->avail_in = len;

	while (zs->avail_in > 0)
	{
		int			res;

		/* Write output data into unused portion of output buffer. */
		Assert(mysink->bytes_written < mysink->base.bbs_next->bbs_buffer_length);
		zs->next_out = (uint8 *)
			mysink->base.bbs_next->bbs_buffer + mysink->bytes_written;
		zs->avail_out =
			mysink->base.bbs_next->bbs_buffer_length - mysink->bytes_written;

		/*
		 * Try to compress. Note that this will update zs->next_in and
		 * zs->avail_in according to how much input data was consumed, and
		 * zs->next_out and zs->avail_out according to how many output bytes
		 * were produced.
		 *
		 * According to the zlib documentation, Z_STREAM_ERROR should only
		 * occur if we've made a programming error, or if say there's been a
		 * memory clobber; we use elog() rather than Assert() here out of an
		 * abundance of caution.
		 */
		res = deflate(zs, Z_NO_FLUSH);
		if (res == Z_STREAM_ERROR)
			elog(ERROR, "could not compress data: %s", zs->msg);

		/* Update our notion of how many bytes we've written. */
		mysink->bytes_written =
			mysink->base.bbs_next->bbs_buffer_length - zs->avail_out;

		/*
		 * If the output buffer is full, it's time for the next sink to
		 * process the contents.
		 */
		if (mysink->bytes_written >= mysink->base.bbs_next->bbs_buffer_length)
		{
			bbsink_archive_contents(sink->bbs_next, mysink->bytes_written);
			mysink->bytes_written = 0;
		}
	}
}

/*
 * There might be some data inside zlib's internal buffers; we need to get
 * that flushed out and forwarded to the successor sink as archive content.
 *
 * Then we can end processing for this archive.
 */
static void
bbsink_gzip_end_archive(bbsink *sink)
{
	bbsink_gzip *mysink = (bbsink_gzip *) sink;
	z_stream   *zs = &mysink->zstream;

	/* There is no more data available. */
	zs->next_in = (uint8 *) mysink->base.bbs_buffer;
	zs->avail_in = 0;

	while (1)
	{
		int			res;

		/* Write output data into unused portion of output buffer. */
		Assert(mysink->bytes_written < mysink->base.bbs_next->bbs_buffer_length);
		zs->next_out = (uint8 *)
			mysink->base.bbs_next->bbs_buffer + mysink->bytes_written;
		zs->avail_out =
			mysink->base.bbs_next->bbs_buffer_length - mysink->bytes_written;

		/*
		 * As bbsink_gzip_archive_contents, but pass Z_FINISH since there is
		 * no more input.
		 */
		res = deflate(zs, Z_FINISH);
		if (res == Z_STREAM_ERROR)
			elog(ERROR, "could not compress data: %s", zs->msg);

		/* Update our notion of how many bytes we've written. */
		mysink->bytes_written =
			mysink->base.bbs_next->bbs_buffer_length - zs->avail_out;

		/*
		 * Apparently we had no data in the output buffer and deflate() was
		 * not able to add any. We must be done.
		 */
		if (mysink->bytes_written == 0)
			break;

		/* Send whatever accumulated output bytes we have. */
		bbsink_archive_contents(sink->bbs_next, mysink->bytes_written);
		mysink->bytes_written = 0;
	}

	/* Must also pass on the information that this archive has ended. */
	bbsink_forward_end_archive(sink);
}

/*
 * Manifest contents are not compressed, but we do need to copy them into
 * the successor sink's buffer, because we have our own.
 */
static void
bbsink_gzip_manifest_contents(bbsink *sink, size_t len)
{
	memcpy(sink->bbs_next->bbs_buffer, sink->bbs_buffer, len);
	bbsink_manifest_contents(sink->bbs_next, len);
}

/*
 * Wrapper function to adjust the signature of palloc to match what libz
 * expects.
 */
static void *
gzip_palloc(void *opaque, unsigned items, unsigned size)
{
	return palloc(items * size);
}

/*
 * Wrapper function to adjust the signature of pfree to match what libz
 * expects.
 */
static void
gzip_pfree(void *opaque, void *address)
{
	pfree(address);
}

#endif
