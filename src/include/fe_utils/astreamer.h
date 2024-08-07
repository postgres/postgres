/*-------------------------------------------------------------------------
 *
 * astreamer.h
 *
 * The "archive streamer" interface is intended to allow frontend code
 * to stream from possibly-compressed archive files from any source and
 * perform arbitrary actions based on the contents of those archives.
 * Archive streamers are intended to be composable, and most tasks will
 * require two or more archive streamers to complete. For instance,
 * if the input is an uncompressed tar stream, a tar parser astreamer
 * could be used to interpret it, and then an extractor astreamer could
 * be used to write each archive member out to a file.
 *
 * In general, each archive streamer is relatively free to take whatever
 * action it desires in the stream of chunks provided by the caller. It
 * may do something simple, like write the archive to a file, perhaps after
 * compressing it, but it can also do more complicated things, like
 * annotating the byte stream to indicate which parts of the data
 * correspond to tar headers or trailing padding, vs. which parts are
 * payload data. A subsequent astreamer may use this information to
 * make further decisions about how to process the data; for example,
 * it might choose to modify the archive contents.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/astreamer.h
 *-------------------------------------------------------------------------
 */

#ifndef ASTREAMER_H
#define ASTREAMER_H

#include "common/compression.h"
#include "lib/stringinfo.h"
#include "pqexpbuffer.h"

struct astreamer;
struct astreamer_ops;
typedef struct astreamer astreamer;
typedef struct astreamer_ops astreamer_ops;

/*
 * Each chunk of archive data passed to a astreamer is classified into one
 * of these categories. When data is initially passed to an archive streamer,
 * each chunk will be categorized as ASTREAMER_UNKNOWN, and the chunks can
 * be of whatever size the caller finds convenient.
 *
 * If the archive is parsed (e.g. see astreamer_tar_parser_new()), then all
 * chunks should be labelled as one of the other types listed here. In
 * addition, there should be exactly one ASTREAMER_MEMBER_HEADER chunk and
 * exactly one ASTREAMER_MEMBER_TRAILER chunk per archive member, even if
 * that means a zero-length call. There can be any number of
 * ASTREAMER_MEMBER_CONTENTS chunks in between those calls. There
 * should exactly ASTREAMER_ARCHIVE_TRAILER chunk, and it should follow the
 * last ASTREAMER_MEMBER_TRAILER chunk.
 *
 * In theory, we could need other classifications here, such as a way of
 * indicating an archive header, but the "tar" format doesn't need anything
 * else, so for the time being there's no point.
 */
typedef enum
{
	ASTREAMER_UNKNOWN,
	ASTREAMER_MEMBER_HEADER,
	ASTREAMER_MEMBER_CONTENTS,
	ASTREAMER_MEMBER_TRAILER,
	ASTREAMER_ARCHIVE_TRAILER,
} astreamer_archive_context;

/*
 * Each chunk of data that is classified as ASTREAMER_MEMBER_HEADER,
 * ASTREAMER_MEMBER_CONTENTS, or ASTREAMER_MEMBER_TRAILER should also
 * pass a pointer to an instance of this struct. The details are expected
 * to be present in the archive header and used to fill the struct, after
 * which all subsequent calls for the same archive member are expected to
 * pass the same details.
 */
typedef struct
{
	char		pathname[MAXPGPATH];
	pgoff_t		size;
	mode_t		mode;
	uid_t		uid;
	gid_t		gid;
	bool		is_directory;
	bool		is_link;
	char		linktarget[MAXPGPATH];
} astreamer_member;

/*
 * Generally, each type of astreamer will define its own struct, but the
 * first element should be 'astreamer base'. A astreamer that does not
 * require any additional private data could use this structure directly.
 *
 * bbs_ops is a pointer to the astreamer_ops object which contains the
 * function pointers appropriate to this type of astreamer.
 *
 * bbs_next is a pointer to the successor astreamer, for those types of
 * astreamer which forward data to a successor. It need not be used and
 * should be set to NULL when not relevant.
 *
 * bbs_buffer is a buffer for accumulating data for temporary storage. Each
 * type of astreamer makes its own decisions about whether and how to use
 * this buffer.
 */
struct astreamer
{
	const astreamer_ops *bbs_ops;
	astreamer  *bbs_next;
	StringInfoData bbs_buffer;
};

/*
 * There are three callbacks for a astreamer. The 'content' callback is
 * called repeatedly, as described in the astreamer_archive_context comments.
 * Then, the 'finalize' callback is called once at the end, to give the
 * astreamer a chance to perform cleanup such as closing files. Finally,
 * because this code is running in a frontend environment where, as of this
 * writing, there are no memory contexts, the 'free' callback is called to
 * release memory. These callbacks should always be invoked using the static
 * inline functions defined below.
 */
struct astreamer_ops
{
	void		(*content) (astreamer *streamer, astreamer_member *member,
							const char *data, int len,
							astreamer_archive_context context);
	void		(*finalize) (astreamer *streamer);
	void		(*free) (astreamer *streamer);
};

/* Send some content to a astreamer. */
static inline void
astreamer_content(astreamer *streamer, astreamer_member *member,
				  const char *data, int len,
				  astreamer_archive_context context)
{
	Assert(streamer != NULL);
	streamer->bbs_ops->content(streamer, member, data, len, context);
}

/* Finalize a astreamer. */
static inline void
astreamer_finalize(astreamer *streamer)
{
	Assert(streamer != NULL);
	streamer->bbs_ops->finalize(streamer);
}

/* Free a astreamer. */
static inline void
astreamer_free(astreamer *streamer)
{
	Assert(streamer != NULL);
	streamer->bbs_ops->free(streamer);
}

/*
 * This is a convenience method for use when implementing a astreamer; it is
 * not for use by outside callers. It adds the amount of data specified by
 * 'nbytes' to the astreamer's buffer and adjusts '*len' and '*data'
 * accordingly.
 */
static inline void
astreamer_buffer_bytes(astreamer *streamer, const char **data, int *len,
					   int nbytes)
{
	Assert(nbytes <= *len);

	appendBinaryStringInfo(&streamer->bbs_buffer, *data, nbytes);
	*len -= nbytes;
	*data += nbytes;
}

/*
 * This is a convenience method for use when implementing a astreamer; it is
 * not for use by outsider callers. It attempts to add enough data to the
 * astreamer's buffer to reach a length of target_bytes and adjusts '*len'
 * and '*data' accordingly. It returns true if the target length has been
 * reached and false otherwise.
 */
static inline bool
astreamer_buffer_until(astreamer *streamer, const char **data, int *len,
					   int target_bytes)
{
	int			buflen = streamer->bbs_buffer.len;

	if (buflen >= target_bytes)
	{
		/* Target length already reached; nothing to do. */
		return true;
	}

	if (buflen + *len < target_bytes)
	{
		/* Not enough data to reach target length; buffer all of it. */
		astreamer_buffer_bytes(streamer, data, len, *len);
		return false;
	}

	/* Buffer just enough to reach the target length. */
	astreamer_buffer_bytes(streamer, data, len, target_bytes - buflen);
	return true;
}

/*
 * Functions for creating astreamer objects of various types. See the header
 * comments for each of these functions for details.
 */
extern astreamer *astreamer_plain_writer_new(char *pathname, FILE *file);
extern astreamer *astreamer_gzip_writer_new(char *pathname, FILE *file,
											pg_compress_specification *compress);
extern astreamer *astreamer_extractor_new(const char *basepath,
										  const char *(*link_map) (const char *),
										  void (*report_output_file) (const char *));

extern astreamer *astreamer_gzip_decompressor_new(astreamer *next);
extern astreamer *astreamer_lz4_compressor_new(astreamer *next,
											   pg_compress_specification *compress);
extern astreamer *astreamer_lz4_decompressor_new(astreamer *next);
extern astreamer *astreamer_zstd_compressor_new(astreamer *next,
												pg_compress_specification *compress);
extern astreamer *astreamer_zstd_decompressor_new(astreamer *next);
extern astreamer *astreamer_tar_parser_new(astreamer *next);
extern astreamer *astreamer_tar_terminator_new(astreamer *next);
extern astreamer *astreamer_tar_archiver_new(astreamer *next);

#endif
