/*-------------------------------------------------------------------------
 *
 * astreamer_tar.c
 *
 * This module implements three types of tar processing. A tar parser
 * expects unlabelled chunks of data (e.g. ASTREAMER_UNKNOWN) and splits
 * it into labelled chunks (any other value of astreamer_archive_context).
 * A tar archiver does the reverse: it takes a bunch of labelled chunks
 * and produces a tarfile, optionally replacing member headers and trailers
 * so that upstream astreamer objects can perform surgery on the tarfile
 * contents without knowing the details of the tar format. A tar terminator
 * just adds two blocks of NUL bytes to the end of the file, since older
 * server versions produce files with this terminator omitted.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/astreamer_tar.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <time.h>

#include "common/logging.h"
#include "fe_utils/astreamer.h"
#include "pgtar.h"

typedef struct astreamer_tar_parser
{
	astreamer	base;
	astreamer_archive_context next_context;
	astreamer_member member;
	size_t		file_bytes_sent;
	size_t		pad_bytes_expected;
} astreamer_tar_parser;

typedef struct astreamer_tar_archiver
{
	astreamer	base;
	bool		rearchive_member;
} astreamer_tar_archiver;

static void astreamer_tar_parser_content(astreamer *streamer,
										 astreamer_member *member,
										 const char *data, int len,
										 astreamer_archive_context context);
static void astreamer_tar_parser_finalize(astreamer *streamer);
static void astreamer_tar_parser_free(astreamer *streamer);
static bool astreamer_tar_header(astreamer_tar_parser *mystreamer);

static const astreamer_ops astreamer_tar_parser_ops = {
	.content = astreamer_tar_parser_content,
	.finalize = astreamer_tar_parser_finalize,
	.free = astreamer_tar_parser_free
};

static void astreamer_tar_archiver_content(astreamer *streamer,
										   astreamer_member *member,
										   const char *data, int len,
										   astreamer_archive_context context);
static void astreamer_tar_archiver_finalize(astreamer *streamer);
static void astreamer_tar_archiver_free(astreamer *streamer);

static const astreamer_ops astreamer_tar_archiver_ops = {
	.content = astreamer_tar_archiver_content,
	.finalize = astreamer_tar_archiver_finalize,
	.free = astreamer_tar_archiver_free
};

static void astreamer_tar_terminator_content(astreamer *streamer,
											 astreamer_member *member,
											 const char *data, int len,
											 astreamer_archive_context context);
static void astreamer_tar_terminator_finalize(astreamer *streamer);
static void astreamer_tar_terminator_free(astreamer *streamer);

static const astreamer_ops astreamer_tar_terminator_ops = {
	.content = astreamer_tar_terminator_content,
	.finalize = astreamer_tar_terminator_finalize,
	.free = astreamer_tar_terminator_free
};

/*
 * Create a astreamer that can parse a stream of content as tar data.
 *
 * The input should be a series of ASTREAMER_UNKNOWN chunks; the astreamer
 * specified by 'next' will receive a series of typed chunks, as per the
 * conventions described in astreamer.h.
 */
astreamer *
astreamer_tar_parser_new(astreamer *next)
{
	astreamer_tar_parser *streamer;

	streamer = palloc0(sizeof(astreamer_tar_parser));
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_tar_parser_ops;
	streamer->base.bbs_next = next;
	initStringInfo(&streamer->base.bbs_buffer);
	streamer->next_context = ASTREAMER_MEMBER_HEADER;

	return &streamer->base;
}

/*
 * Parse unknown content as tar data.
 */
static void
astreamer_tar_parser_content(astreamer *streamer, astreamer_member *member,
							 const char *data, int len,
							 astreamer_archive_context context)
{
	astreamer_tar_parser *mystreamer = (astreamer_tar_parser *) streamer;
	size_t		nbytes;

	/* Expect unparsed input. */
	Assert(member == NULL);
	Assert(context == ASTREAMER_UNKNOWN);

	while (len > 0)
	{
		switch (mystreamer->next_context)
		{
			case ASTREAMER_MEMBER_HEADER:

				/*
				 * If we're expecting an archive member header, accumulate a
				 * full block of data before doing anything further.
				 */
				if (!astreamer_buffer_until(streamer, &data, &len,
											TAR_BLOCK_SIZE))
					return;

				/*
				 * Now we can process the header and get ready to process the
				 * file contents; however, we might find out that what we
				 * thought was the next file header is actually the start of
				 * the archive trailer. Switch modes accordingly.
				 */
				if (astreamer_tar_header(mystreamer))
				{
					if (mystreamer->member.size == 0)
					{
						/* No content; trailer is zero-length. */
						astreamer_content(mystreamer->base.bbs_next,
										  &mystreamer->member,
										  NULL, 0,
										  ASTREAMER_MEMBER_TRAILER);

						/* Expect next header. */
						mystreamer->next_context = ASTREAMER_MEMBER_HEADER;
					}
					else
					{
						/* Expect contents. */
						mystreamer->next_context = ASTREAMER_MEMBER_CONTENTS;
					}
					mystreamer->base.bbs_buffer.len = 0;
					mystreamer->file_bytes_sent = 0;
				}
				else
					mystreamer->next_context = ASTREAMER_ARCHIVE_TRAILER;
				break;

			case ASTREAMER_MEMBER_CONTENTS:

				/*
				 * Send as much content as we have, but not more than the
				 * remaining file length.
				 */
				Assert(mystreamer->file_bytes_sent < mystreamer->member.size);
				nbytes = mystreamer->member.size - mystreamer->file_bytes_sent;
				nbytes = Min(nbytes, len);
				Assert(nbytes > 0);
				astreamer_content(mystreamer->base.bbs_next,
								  &mystreamer->member,
								  data, nbytes,
								  ASTREAMER_MEMBER_CONTENTS);
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
						astreamer_content(mystreamer->base.bbs_next,
										  &mystreamer->member,
										  NULL, 0,
										  ASTREAMER_MEMBER_TRAILER);

						/* Expect next header. */
						mystreamer->next_context = ASTREAMER_MEMBER_HEADER;
					}
					else
					{
						/* Trailer is not zero-length. */
						mystreamer->next_context = ASTREAMER_MEMBER_TRAILER;
					}
					mystreamer->base.bbs_buffer.len = 0;
				}
				break;

			case ASTREAMER_MEMBER_TRAILER:

				/*
				 * If we're expecting an archive member trailer, accumulate
				 * the expected number of padding bytes before sending
				 * anything onward.
				 */
				if (!astreamer_buffer_until(streamer, &data, &len,
											mystreamer->pad_bytes_expected))
					return;

				/* OK, now we can send it. */
				astreamer_content(mystreamer->base.bbs_next,
								  &mystreamer->member,
								  data, mystreamer->pad_bytes_expected,
								  ASTREAMER_MEMBER_TRAILER);

				/* Expect next file header. */
				mystreamer->next_context = ASTREAMER_MEMBER_HEADER;
				mystreamer->base.bbs_buffer.len = 0;
				break;

			case ASTREAMER_ARCHIVE_TRAILER:

				/*
				 * We've seen an end-of-archive indicator, so anything more is
				 * buffered and sent as part of the archive trailer. But we
				 * don't expect more than 2 blocks.
				 */
				astreamer_buffer_bytes(streamer, &data, &len, len);
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
 * next astreamer; it is false if we have reached the archive trailer.
 */
static bool
astreamer_tar_header(astreamer_tar_parser *mystreamer)
{
	bool		has_nonzero_byte = false;
	int			i;
	astreamer_member *member = &mystreamer->member;
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
	 */
	strlcpy(member->pathname, &buffer[TAR_OFFSET_NAME], MAXPGPATH);
	if (member->pathname[0] == '\0')
		pg_fatal("tar member has empty name");
	member->size = read_tar_number(&buffer[TAR_OFFSET_SIZE], 12);
	member->mode = read_tar_number(&buffer[TAR_OFFSET_MODE], 8);
	member->uid = read_tar_number(&buffer[TAR_OFFSET_UID], 8);
	member->gid = read_tar_number(&buffer[TAR_OFFSET_GID], 8);
	member->is_directory =
		(buffer[TAR_OFFSET_TYPEFLAG] == TAR_FILETYPE_DIRECTORY);
	member->is_link =
		(buffer[TAR_OFFSET_TYPEFLAG] == TAR_FILETYPE_SYMLINK);
	if (member->is_link)
		strlcpy(member->linktarget, &buffer[TAR_OFFSET_LINKNAME], 100);

	/* Compute number of padding bytes. */
	mystreamer->pad_bytes_expected = tarPaddingBytesRequired(member->size);

	/* Forward the entire header to the next astreamer. */
	astreamer_content(mystreamer->base.bbs_next, member,
					  buffer, TAR_BLOCK_SIZE,
					  ASTREAMER_MEMBER_HEADER);

	return true;
}

/*
 * End-of-stream processing for a tar parser.
 */
static void
astreamer_tar_parser_finalize(astreamer *streamer)
{
	astreamer_tar_parser *mystreamer = (astreamer_tar_parser *) streamer;

	if (mystreamer->next_context != ASTREAMER_ARCHIVE_TRAILER &&
		(mystreamer->next_context != ASTREAMER_MEMBER_HEADER ||
		 mystreamer->base.bbs_buffer.len > 0))
		pg_fatal("COPY stream ended before last file was finished");

	/* Send the archive trailer, even if empty. */
	astreamer_content(streamer->bbs_next, NULL,
					  streamer->bbs_buffer.data, streamer->bbs_buffer.len,
					  ASTREAMER_ARCHIVE_TRAILER);

	/* Now finalize successor. */
	astreamer_finalize(streamer->bbs_next);
}

/*
 * Free memory associated with a tar parser.
 */
static void
astreamer_tar_parser_free(astreamer *streamer)
{
	pfree(streamer->bbs_buffer.data);
	astreamer_free(streamer->bbs_next);
}

/*
 * Create a astreamer that can generate a tar archive.
 *
 * This is intended to be usable either for generating a brand-new tar archive
 * or for modifying one on the fly. The input should be a series of typed
 * chunks (i.e. not ASTREAMER_UNKNOWN). See also the comments for
 * astreamer_tar_parser_content.
 */
astreamer *
astreamer_tar_archiver_new(astreamer *next)
{
	astreamer_tar_archiver *streamer;

	streamer = palloc0(sizeof(astreamer_tar_archiver));
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_tar_archiver_ops;
	streamer->base.bbs_next = next;

	return &streamer->base;
}

/*
 * Fix up the stream of input chunks to create a valid tar file.
 *
 * If a ASTREAMER_MEMBER_HEADER chunk is of size 0, it is replaced with a
 * newly-constructed tar header. If it is of size TAR_BLOCK_SIZE, it is
 * passed through without change. Any other size is a fatal error (and
 * indicates a bug).
 *
 * Whenever a new ASTREAMER_MEMBER_HEADER chunk is constructed, the
 * corresponding ASTREAMER_MEMBER_TRAILER chunk is also constructed from
 * scratch. Specifically, we construct a block of zero bytes sufficient to
 * pad out to a block boundary, as required by the tar format. Other
 * ASTREAMER_MEMBER_TRAILER chunks are passed through without change.
 *
 * Any ASTREAMER_MEMBER_CONTENTS chunks are passed through without change.
 *
 * The ASTREAMER_ARCHIVE_TRAILER chunk is replaced with two
 * blocks of zero bytes. Not all tar programs require this, but apparently
 * some do. The server does not supply this trailer. If no archive trailer is
 * present, one will be added by astreamer_tar_parser_finalize.
 */
static void
astreamer_tar_archiver_content(astreamer *streamer,
							   astreamer_member *member,
							   const char *data, int len,
							   astreamer_archive_context context)
{
	astreamer_tar_archiver *mystreamer = (astreamer_tar_archiver *) streamer;
	char		buffer[2 * TAR_BLOCK_SIZE];

	Assert(context != ASTREAMER_UNKNOWN);

	if (context == ASTREAMER_MEMBER_HEADER && len != TAR_BLOCK_SIZE)
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
	else if (context == ASTREAMER_MEMBER_TRAILER &&
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
	else if (context == ASTREAMER_ARCHIVE_TRAILER)
	{
		/* Trailer should always be two blocks of zero bytes. */
		memset(buffer, 0, 2 * TAR_BLOCK_SIZE);
		data = buffer;
		len = 2 * TAR_BLOCK_SIZE;
	}

	astreamer_content(streamer->bbs_next, member, data, len, context);
}

/*
 * End-of-stream processing for a tar archiver.
 */
static void
astreamer_tar_archiver_finalize(astreamer *streamer)
{
	astreamer_finalize(streamer->bbs_next);
}

/*
 * Free memory associated with a tar archiver.
 */
static void
astreamer_tar_archiver_free(astreamer *streamer)
{
	astreamer_free(streamer->bbs_next);
	pfree(streamer);
}

/*
 * Create a astreamer that blindly adds two blocks of NUL bytes to the
 * end of an incomplete tarfile that the server might send us.
 */
astreamer *
astreamer_tar_terminator_new(astreamer *next)
{
	astreamer  *streamer;

	streamer = palloc0(sizeof(astreamer));
	*((const astreamer_ops **) &streamer->bbs_ops) =
		&astreamer_tar_terminator_ops;
	streamer->bbs_next = next;

	return streamer;
}

/*
 * Pass all the content through without change.
 */
static void
astreamer_tar_terminator_content(astreamer *streamer,
								 astreamer_member *member,
								 const char *data, int len,
								 astreamer_archive_context context)
{
	/* Expect unparsed input. */
	Assert(member == NULL);
	Assert(context == ASTREAMER_UNKNOWN);

	/* Just forward it. */
	astreamer_content(streamer->bbs_next, member, data, len, context);
}

/*
 * At the end, blindly add the two blocks of NUL bytes which the server fails
 * to supply.
 */
static void
astreamer_tar_terminator_finalize(astreamer *streamer)
{
	char		buffer[2 * TAR_BLOCK_SIZE];

	memset(buffer, 0, 2 * TAR_BLOCK_SIZE);
	astreamer_content(streamer->bbs_next, NULL, buffer,
					  2 * TAR_BLOCK_SIZE, ASTREAMER_UNKNOWN);
	astreamer_finalize(streamer->bbs_next);
}

/*
 * Free memory associated with a tar terminator.
 */
static void
astreamer_tar_terminator_free(astreamer *streamer)
{
	astreamer_free(streamer->bbs_next);
	pfree(streamer);
}
