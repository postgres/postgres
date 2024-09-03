/*-------------------------------------------------------------------------
 *
 * astreamer_gzip.c
 *
 * Archive streamers that deal with data compressed using gzip.
 * astreamer_gzip_writer applies gzip compression to the input data
 * and writes the result to a file. astreamer_gzip_decompressor assumes
 * that the input stream is compressed using gzip and decompresses it.
 *
 * Note that the code in this file is asymmetric with what we do for
 * other compression types: for lz4 and zstd, there is a compressor and
 * a decompressor, rather than a writer and a decompressor. The approach
 * taken here is less flexible, because a writer can only write to a file,
 * while a compressor can write to a subsequent astreamer which is free
 * to do whatever it likes. The reason it's like this is because this
 * code was adapted from old, less-modular pg_basebackup code that used
 * the same APIs that astreamer_gzip_writer now uses, and it didn't seem
 * necessary to change anything at the time.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/astreamer_gzip.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "common/file_perm.h"
#include "common/logging.h"
#include "common/string.h"
#include "fe_utils/astreamer.h"

#ifdef HAVE_LIBZ
typedef struct astreamer_gzip_writer
{
	astreamer	base;
	char	   *pathname;
	gzFile		gzfile;
} astreamer_gzip_writer;

typedef struct astreamer_gzip_decompressor
{
	astreamer	base;
	z_stream	zstream;
	size_t		bytes_written;
} astreamer_gzip_decompressor;

static void astreamer_gzip_writer_content(astreamer *streamer,
										  astreamer_member *member,
										  const char *data, int len,
										  astreamer_archive_context context);
static void astreamer_gzip_writer_finalize(astreamer *streamer);
static void astreamer_gzip_writer_free(astreamer *streamer);
static const char *get_gz_error(gzFile gzf);

static const astreamer_ops astreamer_gzip_writer_ops = {
	.content = astreamer_gzip_writer_content,
	.finalize = astreamer_gzip_writer_finalize,
	.free = astreamer_gzip_writer_free
};

static void astreamer_gzip_decompressor_content(astreamer *streamer,
												astreamer_member *member,
												const char *data, int len,
												astreamer_archive_context context);
static void astreamer_gzip_decompressor_finalize(astreamer *streamer);
static void astreamer_gzip_decompressor_free(astreamer *streamer);
static void *gzip_palloc(void *opaque, unsigned items, unsigned size);
static void gzip_pfree(void *opaque, void *address);

static const astreamer_ops astreamer_gzip_decompressor_ops = {
	.content = astreamer_gzip_decompressor_content,
	.finalize = astreamer_gzip_decompressor_finalize,
	.free = astreamer_gzip_decompressor_free
};
#endif

/*
 * Create a astreamer that just compresses data using gzip, and then writes
 * it to a file.
 *
 * The caller must specify a pathname and may specify a file. The pathname is
 * used for error-reporting purposes either way. If file is NULL, the pathname
 * also identifies the file to which the data should be written: it is opened
 * for writing and closed when done. If file is not NULL, the data is written
 * there.
 *
 * Note that zlib does not use the FILE interface, but operates directly on
 * a duplicate of the underlying fd. Hence, callers must take care if they
 * plan to write any other data to the same FILE, either before or after using
 * this.
 */
astreamer *
astreamer_gzip_writer_new(char *pathname, FILE *file,
						  pg_compress_specification *compress)
{
#ifdef HAVE_LIBZ
	astreamer_gzip_writer *streamer;

	streamer = palloc0(sizeof(astreamer_gzip_writer));
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_gzip_writer_ops;

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
		/*
		 * We must dup the file handle so that gzclose doesn't break the
		 * caller's FILE.  See comment for astreamer_gzip_writer_finalize.
		 */
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
	pg_fatal("this build does not support compression with %s", "gzip");
	return NULL;				/* keep compiler quiet */
#endif
}

#ifdef HAVE_LIBZ
/*
 * Write archive content to gzip file.
 */
static void
astreamer_gzip_writer_content(astreamer *streamer,
							  astreamer_member *member, const char *data,
							  int len, astreamer_archive_context context)
{
	astreamer_gzip_writer *mystreamer;

	mystreamer = (astreamer_gzip_writer *) streamer;

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
 * because libz provides no way of avoiding a close on the underlying file
 * handle. Notice, however, that astreamer_gzip_writer_new() uses dup() to
 * work around this issue, so that the behavior from the caller's viewpoint
 * is the same as for astreamer_plain_writer.
 */
static void
astreamer_gzip_writer_finalize(astreamer *streamer)
{
	astreamer_gzip_writer *mystreamer;

	mystreamer = (astreamer_gzip_writer *) streamer;

	errno = 0;					/* in case gzclose() doesn't set it */
	if (gzclose(mystreamer->gzfile) != 0)
		pg_fatal("could not close compressed file \"%s\": %m",
				 mystreamer->pathname);

	mystreamer->gzfile = NULL;
}

/*
 * Free memory associated with this astreamer.
 */
static void
astreamer_gzip_writer_free(astreamer *streamer)
{
	astreamer_gzip_writer *mystreamer;

	mystreamer = (astreamer_gzip_writer *) streamer;

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
astreamer *
astreamer_gzip_decompressor_new(astreamer *next)
{
#ifdef HAVE_LIBZ
	astreamer_gzip_decompressor *streamer;
	z_stream   *zs;

	Assert(next != NULL);

	streamer = palloc0(sizeof(astreamer_gzip_decompressor));
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_gzip_decompressor_ops;

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
	pg_fatal("this build does not support compression with %s", "gzip");
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
astreamer_gzip_decompressor_content(astreamer *streamer,
									astreamer_member *member,
									const char *data, int len,
									astreamer_archive_context context)
{
	astreamer_gzip_decompressor *mystreamer;
	z_stream   *zs;

	mystreamer = (astreamer_gzip_decompressor *) streamer;

	zs = &mystreamer->zstream;
	zs->next_in = (const uint8 *) data;
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
			astreamer_content(mystreamer->base.bbs_next, member,
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
astreamer_gzip_decompressor_finalize(astreamer *streamer)
{
	astreamer_gzip_decompressor *mystreamer;

	mystreamer = (astreamer_gzip_decompressor *) streamer;

	/*
	 * End of the stream, if there is some pending data in output buffers then
	 * we must forward it to next streamer.
	 */
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
astreamer_gzip_decompressor_free(astreamer *streamer)
{
	astreamer_free(streamer->bbs_next);
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
