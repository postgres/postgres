/*-------------------------------------------------------------------------
 *
 * bbstreamer_gzip.c
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/bbstreamer_gzip.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "bbstreamer.h"
#include "common/logging.h"
#include "common/file_perm.h"
#include "common/string.h"

#ifdef HAVE_LIBZ
typedef struct bbstreamer_gzip_writer
{
	bbstreamer	base;
	char	   *pathname;
	gzFile		gzfile;
} bbstreamer_gzip_writer;

typedef struct bbstreamer_gzip_decompressor
{
	bbstreamer	base;
	z_stream	zstream;
	size_t		bytes_written;
} bbstreamer_gzip_decompressor;

static void bbstreamer_gzip_writer_content(bbstreamer *streamer,
										   bbstreamer_member *member,
										   const char *data, int len,
										   bbstreamer_archive_context context);
static void bbstreamer_gzip_writer_finalize(bbstreamer *streamer);
static void bbstreamer_gzip_writer_free(bbstreamer *streamer);
static const char *get_gz_error(gzFile gzf);

const bbstreamer_ops bbstreamer_gzip_writer_ops = {
	.content = bbstreamer_gzip_writer_content,
	.finalize = bbstreamer_gzip_writer_finalize,
	.free = bbstreamer_gzip_writer_free
};

static void bbstreamer_gzip_decompressor_content(bbstreamer *streamer,
												 bbstreamer_member *member,
												 const char *data, int len,
												 bbstreamer_archive_context context);
static void bbstreamer_gzip_decompressor_finalize(bbstreamer *streamer);
static void bbstreamer_gzip_decompressor_free(bbstreamer *streamer);
static void *gzip_palloc(void *opaque, unsigned items, unsigned size);
static void gzip_pfree(void *opaque, void *address);

const bbstreamer_ops bbstreamer_gzip_decompressor_ops = {
	.content = bbstreamer_gzip_decompressor_content,
	.finalize = bbstreamer_gzip_decompressor_finalize,
	.free = bbstreamer_gzip_decompressor_free
};
#endif

/*
 * Create a bbstreamer that just compresses data using gzip, and then writes
 * it to a file.
 *
 * As in the case of bbstreamer_plain_writer_new, pathname is always used
 * for error reporting purposes; if file is NULL, it is also the opened and
 * closed so that the data may be written there.
 */
bbstreamer *
bbstreamer_gzip_writer_new(char *pathname, FILE *file,
						   pg_compress_specification *compress)
{
#ifdef HAVE_LIBZ
	bbstreamer_gzip_writer *streamer;

	streamer = palloc0(sizeof(bbstreamer_gzip_writer));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_gzip_writer_ops;

	streamer->pathname = pstrdup(pathname);

	if (file == NULL)
	{
		streamer->gzfile = gzopen(pathname, "wb");
		if (streamer->gzfile == NULL)
			pg_fatal("could not create compressed file \"%s\": %m",
					 pathname);
	}
	else
	{
		int			fd = dup(fileno(file));

		if (fd < 0)
			pg_fatal("could not duplicate stdout: %m");

		streamer->gzfile = gzdopen(fd, "wb");
		if (streamer->gzfile == NULL)
			pg_fatal("could not open output file: %m");
	}

	if (gzsetparams(streamer->gzfile, compress->level, Z_DEFAULT_STRATEGY) != Z_OK)
		pg_fatal("could not set compression level %d: %s",
				 compress->level, get_gz_error(streamer->gzfile));

	return &streamer->base;
#else
	pg_fatal("this build does not support gzip compression");
	return NULL;				/* keep compiler quiet */
#endif
}

#ifdef HAVE_LIBZ
/*
 * Write archive content to gzip file.
 */
static void
bbstreamer_gzip_writer_content(bbstreamer *streamer,
							   bbstreamer_member *member, const char *data,
							   int len, bbstreamer_archive_context context)
{
	bbstreamer_gzip_writer *mystreamer;

	mystreamer = (bbstreamer_gzip_writer *) streamer;

	if (len == 0)
		return;

	errno = 0;
	if (gzwrite(mystreamer->gzfile, data, len) != len)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to compressed file \"%s\": %s",
				 mystreamer->pathname, get_gz_error(mystreamer->gzfile));
	}
}

/*
 * End-of-archive processing when writing to a gzip file consists of just
 * calling gzclose.
 *
 * It makes no difference whether we opened the file or the caller did it,
 * because libz provides no way of avoiding a close on the underling file
 * handle. Notice, however, that bbstreamer_gzip_writer_new() uses dup() to
 * work around this issue, so that the behavior from the caller's viewpoint
 * is the same as for bbstreamer_plain_writer.
 */
static void
bbstreamer_gzip_writer_finalize(bbstreamer *streamer)
{
	bbstreamer_gzip_writer *mystreamer;

	mystreamer = (bbstreamer_gzip_writer *) streamer;

	errno = 0;					/* in case gzclose() doesn't set it */
	if (gzclose(mystreamer->gzfile) != 0)
		pg_fatal("could not close compressed file \"%s\": %m",
				 mystreamer->pathname);

	mystreamer->gzfile = NULL;
}

/*
 * Free memory associated with this bbstreamer.
 */
static void
bbstreamer_gzip_writer_free(bbstreamer *streamer)
{
	bbstreamer_gzip_writer *mystreamer;

	mystreamer = (bbstreamer_gzip_writer *) streamer;

	Assert(mystreamer->base.bbs_next == NULL);
	Assert(mystreamer->gzfile == NULL);

	pfree(mystreamer->pathname);
	pfree(mystreamer);
}

/*
 * Helper function for libz error reporting.
 */
static const char *
get_gz_error(gzFile gzf)
{
	int			errnum;
	const char *errmsg;

	errmsg = gzerror(gzf, &errnum);
	if (errnum == Z_ERRNO)
		return strerror(errno);
	else
		return errmsg;
}
#endif

/*
 * Create a new base backup streamer that performs decompression of gzip
 * compressed blocks.
 */
bbstreamer *
bbstreamer_gzip_decompressor_new(bbstreamer *next)
{
#ifdef HAVE_LIBZ
	bbstreamer_gzip_decompressor *streamer;
	z_stream   *zs;

	Assert(next != NULL);

	streamer = palloc0(sizeof(bbstreamer_gzip_decompressor));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_gzip_decompressor_ops;

	streamer->base.bbs_next = next;
	initStringInfo(&streamer->base.bbs_buffer);

	/* Initialize internal stream state for decompression */
	zs = &streamer->zstream;
	zs->zalloc = gzip_palloc;
	zs->zfree = gzip_pfree;
	zs->next_out = (uint8 *) streamer->base.bbs_buffer.data;
	zs->avail_out = streamer->base.bbs_buffer.maxlen;

	/*
	 * Data compression was initialized using deflateInit2 to request a gzip
	 * header. Similarly, we are using inflateInit2 to initialize data
	 * decompression.
	 *
	 * Per the documentation for inflateInit2, the second argument is
	 * "windowBits" and its value must be greater than or equal to the value
	 * provided while compressing the data, so we are using the maximum
	 * possible value for safety.
	 */
	if (inflateInit2(zs, 15 + 16) != Z_OK)
		pg_fatal("could not initialize compression library");

	return &streamer->base;
#else
	pg_fatal("this build does not support gzip compression");
	return NULL;				/* keep compiler quiet */
#endif
}

#ifdef HAVE_LIBZ
/*
 * Decompress the input data to output buffer until we run out of input
 * data. Each time the output buffer is full, pass on the decompressed data
 * to the next streamer.
 */
static void
bbstreamer_gzip_decompressor_content(bbstreamer *streamer,
									 bbstreamer_member *member,
									 const char *data, int len,
									 bbstreamer_archive_context context)
{
	bbstreamer_gzip_decompressor *mystreamer;
	z_stream   *zs;

	mystreamer = (bbstreamer_gzip_decompressor *) streamer;

	zs = &mystreamer->zstream;
	zs->next_in = (uint8 *) data;
	zs->avail_in = len;

	/* Process the current chunk */
	while (zs->avail_in > 0)
	{
		int			res;

		Assert(mystreamer->bytes_written < mystreamer->base.bbs_buffer.maxlen);

		zs->next_out = (uint8 *)
			mystreamer->base.bbs_buffer.data + mystreamer->bytes_written;
		zs->avail_out =
			mystreamer->base.bbs_buffer.maxlen - mystreamer->bytes_written;

		/*
		 * This call decompresses data starting at zs->next_in and updates
		 * zs->next_in * and zs->avail_in. It generates output data starting
		 * at zs->next_out and updates zs->next_out and zs->avail_out
		 * accordingly.
		 */
		res = inflate(zs, Z_NO_FLUSH);

		if (res == Z_STREAM_ERROR)
			pg_log_error("could not decompress data: %s", zs->msg);

		mystreamer->bytes_written =
			mystreamer->base.bbs_buffer.maxlen - zs->avail_out;

		/* If output buffer is full then pass data to next streamer */
		if (mystreamer->bytes_written >= mystreamer->base.bbs_buffer.maxlen)
		{
			bbstreamer_content(mystreamer->base.bbs_next, member,
							   mystreamer->base.bbs_buffer.data,
							   mystreamer->base.bbs_buffer.maxlen, context);
			mystreamer->bytes_written = 0;
		}
	}
}

/*
 * End-of-stream processing.
 */
static void
bbstreamer_gzip_decompressor_finalize(bbstreamer *streamer)
{
	bbstreamer_gzip_decompressor *mystreamer;

	mystreamer = (bbstreamer_gzip_decompressor *) streamer;

	/*
	 * End of the stream, if there is some pending data in output buffers then
	 * we must forward it to next streamer.
	 */
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
bbstreamer_gzip_decompressor_free(bbstreamer *streamer)
{
	bbstreamer_free(streamer->bbs_next);
	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
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
