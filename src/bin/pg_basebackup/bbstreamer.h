/*-------------------------------------------------------------------------
 *
 * bbstreamer.h
 *
 * Each tar archive returned by the server is passed to one or more
 * bbstreamer objects for further processing. The bbstreamer may do
 * something simple, like write the archive to a file, perhaps after
 * compressing it, but it can also do more complicated things, like
 * annotating the byte stream to indicate which parts of the data
 * correspond to tar headers or trailing padding, vs. which parts are
 * payload data. A subsequent bbstreamer may use this information to
 * make further decisions about how to process the data; for example,
 * it might choose to modify the archive contents.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/bbstreamer.h
 *-------------------------------------------------------------------------
 */

#ifndef BBSTREAMER_H
#define BBSTREAMER_H

#include "common/compression.h"
#include "lib/stringinfo.h"
#include "pqexpbuffer.h"

struct bbstreamer;
struct bbstreamer_ops;
typedef struct bbstreamer bbstreamer;
typedef struct bbstreamer_ops bbstreamer_ops;

/*
 * Each chunk of archive data passed to a bbstreamer is classified into one
 * of these categories. When data is first received from the remote server,
 * each chunk will be categorized as BBSTREAMER_UNKNOWN, and the chunks will
 * be of whatever size the remote server chose to send.
 *
 * If the archive is parsed (e.g. see bbstreamer_tar_parser_new()), then all
 * chunks should be labelled as one of the other types listed here. In
 * addition, there should be exactly one BBSTREAMER_MEMBER_HEADER chunk and
 * exactly one BBSTREAMER_MEMBER_TRAILER chunk per archive member, even if
 * that means a zero-length call. There can be any number of
 * BBSTREAMER_MEMBER_CONTENTS chunks in between those calls. There
 * should exactly BBSTREAMER_ARCHIVE_TRAILER chunk, and it should follow the
 * last BBSTREAMER_MEMBER_TRAILER chunk.
 *
 * In theory, we could need other classifications here, such as a way of
 * indicating an archive header, but the "tar" format doesn't need anything
 * else, so for the time being there's no point.
 */
typedef enum
{
	BBSTREAMER_UNKNOWN,
	BBSTREAMER_MEMBER_HEADER,
	BBSTREAMER_MEMBER_CONTENTS,
	BBSTREAMER_MEMBER_TRAILER,
	BBSTREAMER_ARCHIVE_TRAILER
} bbstreamer_archive_context;

/*
 * Each chunk of data that is classified as BBSTREAMER_MEMBER_HEADER,
 * BBSTREAMER_MEMBER_CONTENTS, or BBSTREAMER_MEMBER_TRAILER should also
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
} bbstreamer_member;

/*
 * Generally, each type of bbstreamer will define its own struct, but the
 * first element should be 'bbstreamer base'. A bbstreamer that does not
 * require any additional private data could use this structure directly.
 *
 * bbs_ops is a pointer to the bbstreamer_ops object which contains the
 * function pointers appropriate to this type of bbstreamer.
 *
 * bbs_next is a pointer to the successor bbstreamer, for those types of
 * bbstreamer which forward data to a successor. It need not be used and
 * should be set to NULL when not relevant.
 *
 * bbs_buffer is a buffer for accumulating data for temporary storage. Each
 * type of bbstreamer makes its own decisions about whether and how to use
 * this buffer.
 */
struct bbstreamer
{
	const bbstreamer_ops *bbs_ops;
	bbstreamer *bbs_next;
	StringInfoData bbs_buffer;
};

/*
 * There are three callbacks for a bbstreamer. The 'content' callback is
 * called repeatedly, as described in the bbstreamer_archive_context comments.
 * Then, the 'finalize' callback is called once at the end, to give the
 * bbstreamer a chance to perform cleanup such as closing files. Finally,
 * because this code is running in a frontend environment where, as of this
 * writing, there are no memory contexts, the 'free' callback is called to
 * release memory. These callbacks should always be invoked using the static
 * inline functions defined below.
 */
struct bbstreamer_ops
{
	void		(*content) (bbstreamer *streamer, bbstreamer_member *member,
							const char *data, int len,
							bbstreamer_archive_context context);
	void		(*finalize) (bbstreamer *streamer);
	void		(*free) (bbstreamer *streamer);
};

/* Send some content to a bbstreamer. */
static inline void
bbstreamer_content(bbstreamer *streamer, bbstreamer_member *member,
				   const char *data, int len,
				   bbstreamer_archive_context context)
{
	Assert(streamer != NULL);
	streamer->bbs_ops->content(streamer, member, data, len, context);
}

/* Finalize a bbstreamer. */
static inline void
bbstreamer_finalize(bbstreamer *streamer)
{
	Assert(streamer != NULL);
	streamer->bbs_ops->finalize(streamer);
}

/* Free a bbstreamer. */
static inline void
bbstreamer_free(bbstreamer *streamer)
{
	Assert(streamer != NULL);
	streamer->bbs_ops->free(streamer);
}

/*
 * This is a convenience method for use when implementing a bbstreamer; it is
 * not for use by outside callers. It adds the amount of data specified by
 * 'nbytes' to the bbstreamer's buffer and adjusts '*len' and '*data'
 * accordingly.
 */
static inline void
bbstreamer_buffer_bytes(bbstreamer *streamer, const char **data, int *len,
						int nbytes)
{
	Assert(nbytes <= *len);

	appendBinaryStringInfo(&streamer->bbs_buffer, *data, nbytes);
	*len -= nbytes;
	*data += nbytes;
}

/*
 * This is a convenence method for use when implementing a bbstreamer; it is
 * not for use by outsider callers. It attempts to add enough data to the
 * bbstreamer's buffer to reach a length of target_bytes and adjusts '*len'
 * and '*data' accordingly. It returns true if the target length has been
 * reached and false otherwise.
 */
static inline bool
bbstreamer_buffer_until(bbstreamer *streamer, const char **data, int *len,
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
		bbstreamer_buffer_bytes(streamer, data, len, *len);
		return false;
	}

	/* Buffer just enough to reach the target length. */
	bbstreamer_buffer_bytes(streamer, data, len, target_bytes - buflen);
	return true;
}

/*
 * Functions for creating bbstreamer objects of various types. See the header
 * comments for each of these functions for details.
 */
extern bbstreamer *bbstreamer_plain_writer_new(char *pathname, FILE *file);
extern bbstreamer *bbstreamer_gzip_writer_new(char *pathname, FILE *file,
											  pg_compress_specification *compress);
extern bbstreamer *bbstreamer_extractor_new(const char *basepath,
											const char *(*link_map) (const char *),
											void (*report_output_file) (const char *));

extern bbstreamer *bbstreamer_gzip_decompressor_new(bbstreamer *next);
extern bbstreamer *bbstreamer_lz4_compressor_new(bbstreamer *next,
												 pg_compress_specification *compress);
extern bbstreamer *bbstreamer_lz4_decompressor_new(bbstreamer *next);
extern bbstreamer *bbstreamer_zstd_compressor_new(bbstreamer *next,
												  pg_compress_specification *compress);
extern bbstreamer *bbstreamer_zstd_decompressor_new(bbstreamer *next);
extern bbstreamer *bbstreamer_tar_parser_new(bbstreamer *next);
extern bbstreamer *bbstreamer_tar_terminator_new(bbstreamer *next);
extern bbstreamer *bbstreamer_tar_archiver_new(bbstreamer *next);

extern bbstreamer *bbstreamer_recovery_injector_new(bbstreamer *next,
													bool is_recovery_guc_supported,
													PQExpBuffer recoveryconfcontents);
extern void bbstreamer_inject_file(bbstreamer *streamer, char *pathname,
								   char *data, int len);

#endif
