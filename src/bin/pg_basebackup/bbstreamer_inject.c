/*-------------------------------------------------------------------------
 *
 * bbstreamer_inject.c
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/bbstreamer_inject.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "bbstreamer.h"
#include "common/file_perm.h"
#include "common/logging.h"

typedef struct bbstreamer_recovery_injector
{
	bbstreamer	base;
	bool		skip_file;
	bool		is_recovery_guc_supported;
	bool		is_postgresql_auto_conf;
	bool		found_postgresql_auto_conf;
	PQExpBuffer recoveryconfcontents;
	bbstreamer_member member;
} bbstreamer_recovery_injector;

static void bbstreamer_recovery_injector_content(bbstreamer *streamer,
												 bbstreamer_member *member,
												 const char *data, int len,
												 bbstreamer_archive_context context);
static void bbstreamer_recovery_injector_finalize(bbstreamer *streamer);
static void bbstreamer_recovery_injector_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_recovery_injector_ops = {
	.content = bbstreamer_recovery_injector_content,
	.finalize = bbstreamer_recovery_injector_finalize,
	.free = bbstreamer_recovery_injector_free
};

/*
 * Create a bbstreamer that can edit recoverydata into an archive stream.
 *
 * The input should be a series of typed chunks (not BBSTREAMER_UNKNOWN) as
 * per the conventions described in bbstreamer.h; the chunks forwarded to
 * the next bbstreamer will be similarly typed, but the
 * BBSTREAMER_MEMBER_HEADER chunks may be zero-length in cases where we've
 * edited the archive stream.
 *
 * Our goal is to do one of the following three things with the content passed
 * via recoveryconfcontents: (1) if is_recovery_guc_supported is false, then
 * put the content into recovery.conf, replacing any existing archive member
 * by that name; (2) if is_recovery_guc_supported is true and
 * postgresql.auto.conf exists in the archive, then append the content
 * provided to the existing file; and (3) if is_recovery_guc_supported is
 * true but postgresql.auto.conf does not exist in the archive, then create
 * it with the specified content.
 *
 * In addition, if is_recovery_guc_supported is true, then we create a
 * zero-length standby.signal file, dropping any file with that name from
 * the archive.
 */
extern bbstreamer *
bbstreamer_recovery_injector_new(bbstreamer *next,
								 bool is_recovery_guc_supported,
								 PQExpBuffer recoveryconfcontents)
{
	bbstreamer_recovery_injector *streamer;

	streamer = palloc0(sizeof(bbstreamer_recovery_injector));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_recovery_injector_ops;
	streamer->base.bbs_next = next;
	streamer->is_recovery_guc_supported = is_recovery_guc_supported;
	streamer->recoveryconfcontents = recoveryconfcontents;

	return &streamer->base;
}

/*
 * Handle each chunk of tar content while injecting recovery configuration.
 */
static void
bbstreamer_recovery_injector_content(bbstreamer *streamer,
									 bbstreamer_member *member,
									 const char *data, int len,
									 bbstreamer_archive_context context)
{
	bbstreamer_recovery_injector *mystreamer;

	mystreamer = (bbstreamer_recovery_injector *) streamer;
	Assert(member != NULL || context == BBSTREAMER_ARCHIVE_TRAILER);

	switch (context)
	{
		case BBSTREAMER_MEMBER_HEADER:
			/* Must copy provided data so we have the option to modify it. */
			memcpy(&mystreamer->member, member, sizeof(bbstreamer_member));

			/*
			 * On v12+, skip standby.signal and edit postgresql.auto.conf; on
			 * older versions, skip recovery.conf.
			 */
			if (mystreamer->is_recovery_guc_supported)
			{
				mystreamer->skip_file =
					(strcmp(member->pathname, "standby.signal") == 0);
				mystreamer->is_postgresql_auto_conf =
					(strcmp(member->pathname, "postgresql.auto.conf") == 0);
				if (mystreamer->is_postgresql_auto_conf)
				{
					/* Remember we saw it so we don't add it again. */
					mystreamer->found_postgresql_auto_conf = true;

					/* Increment length by data to be injected. */
					mystreamer->member.size +=
						mystreamer->recoveryconfcontents->len;

					/*
					 * Zap data and len because the archive header is no
					 * longer valid; some subsequent bbstreamer must
					 * regenerate it if it's necessary.
					 */
					data = NULL;
					len = 0;
				}
			}
			else
				mystreamer->skip_file =
					(strcmp(member->pathname, "recovery.conf") == 0);

			/* Do not forward if the file is to be skipped. */
			if (mystreamer->skip_file)
				return;
			break;

		case BBSTREAMER_MEMBER_CONTENTS:
			/* Do not forward if the file is to be skipped. */
			if (mystreamer->skip_file)
				return;
			break;

		case BBSTREAMER_MEMBER_TRAILER:
			/* Do not forward it the file is to be skipped. */
			if (mystreamer->skip_file)
				return;

			/* Append provided content to whatever we already sent. */
			if (mystreamer->is_postgresql_auto_conf)
				bbstreamer_content(mystreamer->base.bbs_next, member,
								   mystreamer->recoveryconfcontents->data,
								   mystreamer->recoveryconfcontents->len,
								   BBSTREAMER_MEMBER_CONTENTS);
			break;

		case BBSTREAMER_ARCHIVE_TRAILER:
			if (mystreamer->is_recovery_guc_supported)
			{
				/*
				 * If we didn't already find (and thus modify)
				 * postgresql.auto.conf, inject it as an additional archive
				 * member now.
				 */
				if (!mystreamer->found_postgresql_auto_conf)
					bbstreamer_inject_file(mystreamer->base.bbs_next,
										   "postgresql.auto.conf",
										   mystreamer->recoveryconfcontents->data,
										   mystreamer->recoveryconfcontents->len);

				/* Inject empty standby.signal file. */
				bbstreamer_inject_file(mystreamer->base.bbs_next,
									   "standby.signal", "", 0);
			}
			else
			{
				/* Inject recovery.conf file with specified contents. */
				bbstreamer_inject_file(mystreamer->base.bbs_next,
									   "recovery.conf",
									   mystreamer->recoveryconfcontents->data,
									   mystreamer->recoveryconfcontents->len);
			}

			/* Nothing to do here. */
			break;

		default:
			/* Shouldn't happen. */
			pg_fatal("unexpected state while injecting recovery settings");
	}

	bbstreamer_content(mystreamer->base.bbs_next, &mystreamer->member,
					   data, len, context);
}

/*
 * End-of-stream processing for this bbstreamer.
 */
static void
bbstreamer_recovery_injector_finalize(bbstreamer *streamer)
{
	bbstreamer_finalize(streamer->bbs_next);
}

/*
 * Free memory associated with this bbstreamer.
 */
static void
bbstreamer_recovery_injector_free(bbstreamer *streamer)
{
	bbstreamer_free(streamer->bbs_next);
	pfree(streamer);
}

/*
 * Inject a member into the archive with specified contents.
 */
void
bbstreamer_inject_file(bbstreamer *streamer, char *pathname, char *data,
					   int len)
{
	bbstreamer_member member;

	strlcpy(member.pathname, pathname, MAXPGPATH);
	member.size = len;
	member.mode = pg_file_create_mode;
	member.is_directory = false;
	member.is_link = false;
	member.linktarget[0] = '\0';

	/*
	 * There seems to be no principled argument for these values, but they are
	 * what PostgreSQL has historically used.
	 */
	member.uid = 04000;
	member.gid = 02000;

	/*
	 * We don't know here how to generate valid member headers and trailers
	 * for the archiving format in use, so if those are needed, some successor
	 * bbstreamer will have to generate them using the data from 'member'.
	 */
	bbstreamer_content(streamer, &member, NULL, 0,
					   BBSTREAMER_MEMBER_HEADER);
	bbstreamer_content(streamer, &member, data, len,
					   BBSTREAMER_MEMBER_CONTENTS);
	bbstreamer_content(streamer, &member, NULL, 0,
					   BBSTREAMER_MEMBER_TRAILER);
}
