/*-------------------------------------------------------------------------
 *
 * bbstreamer_tar.c
 *
 * This module implements three types of tar processing. A tar parser
 * expects unlabelled chunks of data (e.g. BBSTREAMER_UNKNOWN) and splits
 * it into labelled chunks (any other value of bbstreamer_archive_context).
 * A tar archiver does the reverse: it takes a bunch of labelled chunks
 * and produces a tarfile, optionally replacing member headers and trailers
 * so that upstream bbstreamer objects can perform surgery on the tarfile
 * contents without knowing the details of the tar format. A tar terminator
 * just adds two blocks of NUL bytes to the end of the file, since older
 * server versions produce files with this terminator omitted.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/bbstreamer_tar.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <time.h>

#include "bbstreamer.h"
#include "common/logging.h"
#include "pgtar.h"

typedef struct bbstreamer_tar_parser
{
	bbstreamer	base;
	bbstreamer_archive_context next_context;
	bbstreamer_member member;
	size_t		file_bytes_sent;
	size_t		pad_bytes_expected;
} bbstreamer_tar_parser;

typedef struct bbstreamer_tar_archiver
{
	bbstreamer	base;
	bool		rearchive_member;
} bbstreamer_tar_archiver;

static void bbstreamer_tar_parser_content(bbstreamer *streamer,
										  bbstreamer_member *member,
										  const char *data, int len,
										  bbstreamer_archive_context context);
static void bbstreamer_tar_parser_finalize(bbstreamer *streamer);
static void bbstreamer_tar_parser_free(bbstreamer *streamer);
static bool bbstreamer_tar_header(bbstreamer_tar_parser *mystreamer);

const bbstreamer_ops bbstreamer_tar_parser_ops = {
	.content = bbstreamer_tar_parser_content,
	.finalize = bbstreamer_tar_parser_finalize,
	.free = bbstreamer_tar_parser_free
};

static void bbstreamer_tar_archiver_content(bbstreamer *streamer,
											bbstreamer_member *member,
											const char *data, int len,
											bbstreamer_archive_context context);
static void bbstreamer_tar_archiver_finalize(bbstreamer *streamer);
static void bbstreamer_tar_archiver_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_tar_archiver_ops = {
	.content = bbstreamer_tar_archiver_content,
	.finalize = bbstreamer_tar_archiver_finalize,
	.free = bbstreamer_tar_archiver_free
};

static void bbstreamer_tar_terminator_content(bbstreamer *streamer,
											  bbstreamer_member *member,
											  const char *data, int len,
											  bbstreamer_archive_context context);
static void bbstreamer_tar_terminator_finalize(bbstreamer *streamer);
static void bbstreamer_tar_terminator_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_tar_terminator_ops = {
	.content = bbstreamer_tar_terminator_content,
	.finalize = bbstreamer_tar_terminator_finalize,
	.free = bbstreamer_tar_terminator_free
};

/*
 * Create a bbstreamer that can parse a stream of content as tar data.
 *
 * The input should be a series of BBSTREAMER_UNKNOWN chunks; the bbstreamer
 * specified by 'next' will receive a series of typed chunks, as per the
 * conventions described in bbstreamer.h.
 */
extern bbstreamer *
bbstreamer_tar_parser_new(bbstreamer *next)
{
	bbstreamer_tar_parser *streamer;

	streamer = palloc0(sizeof(bbstreamer_tar_parser));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_tar_parser_ops;
	streamer->base.bbs_next = next;
	initStringInfo(&streamer->base.bbs_buffer);
	streamer->next_context = BBSTREAMER_MEMBER_HEADER;

	return &streamer->base;
}

/*
 * Parse unknown content as tar data.
 */
static void
bbstreamer_tar_parser_content(bbstreamer *streamer, bbstreamer_member *member,
							  const char *data, int len,
							  bbstreamer_archive_context context)
{
	bbstreamer_tar_parser *mystreamer = (bbstreamer_tar_parser *) streamer;
	size_t		nbytes;

	/* Expect unparsed input. */
	Assert(member == NULL);
	Assert(context == BBSTREAMER_UNKNOWN);

	while (len > 0)
	{
		switch (mystreamer->next_context)
		{
			case BBSTREAMER_MEMBER_HEADER:

				/*
				 * If we're expecting an archive member header, accumulate a
				 * full block of data before doing anything further.
				 */
				if (!bbstreamer_buffer_until(streamer, &data, &len,
											 TAR_BLOCK_SIZE))
					return;

				/*
				 * Now we can process the header and get ready to process the
				 * file contents; however, we might find out that what we
				 * thought was the next file header is actually the start of
				 * the archive trailer. Switch modes accordingly.
				 */
				if (bbstreamer_tar_header(mystreamer))
				{
					if (mystreamer->member.size == 0)
					{
						/* No content; trailer is zero-length. */
						bbstreamer_content(mystreamer->base.bbs_next,
										   &mystreamer->member,
										   NULL, 0,
										   BBSTREAMER_MEMBER_TRAILER);

						/* Expect next header. */
						mystreamer->next_context = BBSTREAMER_MEMBER_HEADER;
					}
					else
					{
						/* Expect contents. */
						mystreamer->next_context = BBSTREAMER_MEMBER_CONTENTS;
					}
					mystreamer->base.bbs_buffer.len = 0;
					mystreamer->file_bytes_sent = 0;
				}
				else
					mystreamer->next_context = BBSTREAMER_ARCHIVE_TRAILER;
				break;

			case BBSTREAMER_MEMBER_CONTENTS:

				/*
				 * Send as much content as we have, but not more than the
				 * remaining file length.
				 */
				Assert(mystreamer->file_bytes_sent < mystreamer->member.size);
				nbytes = mystreamer->member.size - mystreamer->file_bytes_sent;
				nbytes = Min(nbytes, len);
				Assert(nbytes > 0);
				bbstreamer_content(mystreamer->base.bbs_next,
								   &mystreamer->member,
								   data, nbytes,
								   BBSTREAMER_MEMBER_CONTENTS);
				mystreamer->file_bytes_sent += nbytes;
				data += nbytes;
				len -= nbytes;

				/*
				 * If we've not yet sent the whole file, then there's more
				 * content to come; otherwise, it's time to expect the file
				 * trailer.
				 */
				Assert(mystreamer->file_bytes_sent <= mystreamer->member.size);
				if (mystreamer->file_bytes_sent == mystreamer->member.size)
				{
					if (mystreamer->pad_bytes_expected == 0)
					{
						/* Trailer is zero-length. */
						bbstreamer_content(mystreamer->base.bbs_next,
										   &mystreamer->member,
										   NULL, 0,
										   BBSTREAMER_MEMBER_TRAILER);

						/* Expect next header. */
						mystreamer->next_context = BBSTREAMER_MEMBER_HEADER;
					}
					else
					{
						/* Trailer is not zero-length. */
						mystreamer->next_context = BBSTREAMER_MEMBER_TRAILER;
					}
					mystreamer->base.bbs_buffer.len = 0;
				}
				break;

			case BBSTREAMER_MEMBER_TRAILER:

				/*
				 * If we're expecting an archive member trailer, accumulate
				 * the expected number of padding bytes before sending
				 * anything onward.
				 */
				if (!bbstreamer_buffer_until(streamer, &data, &len,
											 mystreamer->pad_bytes_expected))
					return;

				/* OK, now we can send it. */
				bbstreamer_content(mystreamer->base.bbs_next,
								   &mystreamer->member,
								   data, mystreamer->pad_bytes_expected,
								   BBSTREAMER_MEMBER_TRAILER);

				/* Expect next file header. */
				mystreamer->next_context = BBSTREAMER_MEMBER_HEADER;
				mystreamer->base.bbs_buffer.len = 0;
				break;

			case BBSTREAMER_ARCHIVE_TRAILER:

				/*
				 * We've seen an end-of-archive indicator, so anything more is
				 * buffered and sent as part of the archive trailer. But we
				 * don't expect more than 2 blocks.
				 */
				bbstreamer_buffer_bytes(streamer, &data, &len, len);
				if (len > 2 * TAR_BLOCK_SIZE)
					pg_fatal("tar file trailer exceeds 2 blocks");
				return;

			default:
				/* Shouldn't happen. */
				pg_fatal("unexpected state while parsing tar archive");
		}
	}
}

/*
 * Parse a file header within a tar stream.
 *
 * The return value is true if we found a file header and passed it on to the
 * next bbstreamer; it is false if we have reached the archive trailer.
 */
static bool
bbstreamer_tar_header(bbstreamer_tar_parser *mystreamer)
{
	bool		has_nonzero_byte = false;
	int			i;
	bbstreamer_member *member = &mystreamer->member;
	char	   *buffer = mystreamer->base.bbs_buffer.data;

	Assert(mystreamer->base.bbs_buffer.len == TAR_BLOCK_SIZE);

	/* Check whether we've got a block of all zero bytes. */
	for (i = 0; i < TAR_BLOCK_SIZE; ++i)
	{
		if (buffer[i] != '\0')
		{
			has_nonzero_byte = true;
			break;
		}
	}

	/*
	 * If the entire block was zeros, this is the end of the archive, not the
	 * start of the next file.
	 */
	if (!has_nonzero_byte)
		return false;

	/*
	 * Parse key fields out of the header.
	 *
	 * FIXME: It's terrible that we use hard-coded values here instead of some
	 * more principled approach. It's been like this for a long time, but we
	 * ought to do better.
	 */
	strlcpy(member->pathname, &buffer[0], MAXPGPATH);
	if (member->pathname[0] == '\0')
		pg_fatal("tar member has empty name");
	member->size = read_tar_number(&buffer[124], 12);
	member->mode = read_tar_number(&buffer[100], 8);
	member->uid = read_tar_number(&buffer[108], 8);
	member->gid = read_tar_number(&buffer[116], 8);
	member->is_directory = (buffer[156] == '5');
	member->is_link = (buffer[156] == '2');
	if (member->is_link)
		strlcpy(member->linktarget, &buffer[157], 100);

	/* Compute number of padding bytes. */
	mystreamer->pad_bytes_expected = tarPaddingBytesRequired(member->size);

	/* Forward the entire header to the next bbstreamer. */
	bbstreamer_content(mystreamer->base.bbs_next, member,
					   buffer, TAR_BLOCK_SIZE,
					   BBSTREAMER_MEMBER_HEADER);

	return true;
}

/*
 * End-of-stream processing for a tar parser.
 */
static void
bbstreamer_tar_parser_finalize(bbstreamer *streamer)
{
	bbstreamer_tar_parser *mystreamer = (bbstreamer_tar_parser *) streamer;

	if (mystreamer->next_context != BBSTREAMER_ARCHIVE_TRAILER &&
		(mystreamer->next_context != BBSTREAMER_MEMBER_HEADER ||
		 mystreamer->base.bbs_buffer.len > 0))
		pg_fatal("COPY stream ended before last file was finished");

	/* Send the archive trailer, even if empty. */
	bbstreamer_content(streamer->bbs_next, NULL,
					   streamer->bbs_buffer.data, streamer->bbs_buffer.len,
					   BBSTREAMER_ARCHIVE_TRAILER);

	/* Now finalize successor. */
	bbstreamer_finalize(streamer->bbs_next);
}

/*
 * Free memory associated with a tar parser.
 */
static void
bbstreamer_tar_parser_free(bbstreamer *streamer)
{
	pfree(streamer->bbs_buffer.data);
	bbstreamer_free(streamer->bbs_next);
}

/*
 * Create an bbstreamer that can generate a tar archive.
 *
 * This is intended to be usable either for generating a brand-new tar archive
 * or for modifying one on the fly. The input should be a series of typed
 * chunks (i.e. not BBSTREAMER_UNKNOWN). See also the comments for
 * bbstreamer_tar_parser_content.
 */
extern bbstreamer *
bbstreamer_tar_archiver_new(bbstreamer *next)
{
	bbstreamer_tar_archiver *streamer;

	streamer = palloc0(sizeof(bbstreamer_tar_archiver));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_tar_archiver_ops;
	streamer->base.bbs_next = next;

	return &streamer->base;
}

/*
 * Fix up the stream of input chunks to create a valid tar file.
 *
 * If a BBSTREAMER_MEMBER_HEADER chunk is of size 0, it is replaced with a
 * newly-constructed tar header. If it is of size TAR_BLOCK_SIZE, it is
 * passed through without change. Any other size is a fatal error (and
 * indicates a bug).
 *
 * Whenever a new BBSTREAMER_MEMBER_HEADER chunk is constructed, the
 * corresponding BBSTREAMER_MEMBER_TRAILER chunk is also constructed from
 * scratch. Specifically, we construct a block of zero bytes sufficient to
 * pad out to a block boundary, as required by the tar format. Other
 * BBSTREAMER_MEMBER_TRAILER chunks are passed through without change.
 *
 * Any BBSTREAMER_MEMBER_CONTENTS chunks are passed through without change.
 *
 * The BBSTREAMER_ARCHIVE_TRAILER chunk is replaced with two
 * blocks of zero bytes. Not all tar programs require this, but apparently
 * some do. The server does not supply this trailer. If no archive trailer is
 * present, one will be added by bbstreamer_tar_parser_finalize.
 */
static void
bbstreamer_tar_archiver_content(bbstreamer *streamer,
								bbstreamer_member *member,
								const char *data, int len,
								bbstreamer_archive_context context)
{
	bbstreamer_tar_archiver *mystreamer = (bbstreamer_tar_archiver *) streamer;
	char		buffer[2 * TAR_BLOCK_SIZE];

	Assert(context != BBSTREAMER_UNKNOWN);

	if (context == BBSTREAMER_MEMBER_HEADER && len != TAR_BLOCK_SIZE)
	{
		Assert(len == 0);

		/* Replace zero-length tar header with a newly constructed one. */
		tarCreateHeader(buffer, member->pathname, NULL,
						member->size, member->mode, member->uid, member->gid,
						time(NULL));
		data = buffer;
		len = TAR_BLOCK_SIZE;

		/* Also make a note to replace padding, in case size changed. */
		mystreamer->rearchive_member = true;
	}
	else if (context == BBSTREAMER_MEMBER_TRAILER &&
			 mystreamer->rearchive_member)
	{
		int			pad_bytes = tarPaddingBytesRequired(member->size);

		/* Also replace padding, if we regenerated the header. */
		memset(buffer, 0, pad_bytes);
		data = buffer;
		len = pad_bytes;

		/* Don't do this again unless we replace another header. */
		mystreamer->rearchive_member = false;
	}
	else if (context == BBSTREAMER_ARCHIVE_TRAILER)
	{
		/* Trailer should always be two blocks of zero bytes. */
		memset(buffer, 0, 2 * TAR_BLOCK_SIZE);
		data = buffer;
		len = 2 * TAR_BLOCK_SIZE;
	}

	bbstreamer_content(streamer->bbs_next, member, data, len, context);
}

/*
 * End-of-stream processing for a tar archiver.
 */
static void
bbstreamer_tar_archiver_finalize(bbstreamer *streamer)
{
	bbstreamer_finalize(streamer->bbs_next);
}

/*
 * Free memory associated with a tar archiver.
 */
static void
bbstreamer_tar_archiver_free(bbstreamer *streamer)
{
	bbstreamer_free(streamer->bbs_next);
	pfree(streamer);
}

/*
 * Create a bbstreamer that blindly adds two blocks of NUL bytes to the
 * end of an incomplete tarfile that the server might send us.
 */
bbstreamer *
bbstreamer_tar_terminator_new(bbstreamer *next)
{
	bbstreamer *streamer;

	streamer = palloc0(sizeof(bbstreamer));
	*((const bbstreamer_ops **) &streamer->bbs_ops) =
		&bbstreamer_tar_terminator_ops;
	streamer->bbs_next = next;

	return streamer;
}

/*
 * Pass all the content through without change.
 */
static void
bbstreamer_tar_terminator_content(bbstreamer *streamer,
								  bbstreamer_member *member,
								  const char *data, int len,
								  bbstreamer_archive_context context)
{
	/* Expect unparsed input. */
	Assert(member == NULL);
	Assert(context == BBSTREAMER_UNKNOWN);

	/* Just forward it. */
	bbstreamer_content(streamer->bbs_next, member, data, len, context);
}

/*
 * At the end, blindly add the two blocks of NUL bytes which the server fails
 * to supply.
 */
static void
bbstreamer_tar_terminator_finalize(bbstreamer *streamer)
{
	char		buffer[2 * TAR_BLOCK_SIZE];

	memset(buffer, 0, 2 * TAR_BLOCK_SIZE);
	bbstreamer_content(streamer->bbs_next, NULL, buffer,
					   2 * TAR_BLOCK_SIZE, BBSTREAMER_UNKNOWN);
	bbstreamer_finalize(streamer->bbs_next);
}

/*
 * Free memory associated with a tar terminator.
 */
static void
bbstreamer_tar_terminator_free(bbstreamer *streamer)
{
	bbstreamer_free(streamer->bbs_next);
	pfree(streamer);
}
