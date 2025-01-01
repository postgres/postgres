/*-------------------------------------------------------------------------
 *
 * astreamer_inject.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/astreamer_inject.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "astreamer_inject.h"
#include "common/file_perm.h"
#include "common/logging.h"

typedef struct astreamer_recovery_injector
{
	astreamer	base;
	bool		skip_file;
	bool		is_recovery_guc_supported;
	bool		is_postgresql_auto_conf;
	bool		found_postgresql_auto_conf;
	PQExpBuffer recoveryconfcontents;
	astreamer_member member;
} astreamer_recovery_injector;

static void astreamer_recovery_injector_content(astreamer *streamer,
												astreamer_member *member,
												const char *data, int len,
												astreamer_archive_context context);
static void astreamer_recovery_injector_finalize(astreamer *streamer);
static void astreamer_recovery_injector_free(astreamer *streamer);

static const astreamer_ops astreamer_recovery_injector_ops = {
	.content = astreamer_recovery_injector_content,
	.finalize = astreamer_recovery_injector_finalize,
	.free = astreamer_recovery_injector_free
};

/*
 * Create a astreamer that can edit recoverydata into an archive stream.
 *
 * The input should be a series of typed chunks (not ASTREAMER_UNKNOWN) as
 * per the conventions described in astreamer.h; the chunks forwarded to
 * the next astreamer will be similarly typed, but the
 * ASTREAMER_MEMBER_HEADER chunks may be zero-length in cases where we've
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
astreamer *
astreamer_recovery_injector_new(astreamer *next,
								bool is_recovery_guc_supported,
								PQExpBuffer recoveryconfcontents)
{
	astreamer_recovery_injector *streamer;

	streamer = palloc0(sizeof(astreamer_recovery_injector));
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_recovery_injector_ops;
	streamer->base.bbs_next = next;
	streamer->is_recovery_guc_supported = is_recovery_guc_supported;
	streamer->recoveryconfcontents = recoveryconfcontents;

	return &streamer->base;
}

/*
 * Handle each chunk of tar content while injecting recovery configuration.
 */
static void
astreamer_recovery_injector_content(astreamer *streamer,
									astreamer_member *member,
									const char *data, int len,
									astreamer_archive_context context)
{
	astreamer_recovery_injector *mystreamer;

	mystreamer = (astreamer_recovery_injector *) streamer;
	Assert(member != NULL || context == ASTREAMER_ARCHIVE_TRAILER);

	switch (context)
	{
		case ASTREAMER_MEMBER_HEADER:
			/* Must copy provided data so we have the option to modify it. */
			memcpy(&mystreamer->member, member, sizeof(astreamer_member));

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
					 * longer valid; some subsequent astreamer must regenerate
					 * it if it's necessary.
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

		case ASTREAMER_MEMBER_CONTENTS:
			/* Do not forward if the file is to be skipped. */
			if (mystreamer->skip_file)
				return;
			break;

		case ASTREAMER_MEMBER_TRAILER:
			/* Do not forward it the file is to be skipped. */
			if (mystreamer->skip_file)
				return;

			/* Append provided content to whatever we already sent. */
			if (mystreamer->is_postgresql_auto_conf)
				astreamer_content(mystreamer->base.bbs_next, member,
								  mystreamer->recoveryconfcontents->data,
								  mystreamer->recoveryconfcontents->len,
								  ASTREAMER_MEMBER_CONTENTS);
			break;

		case ASTREAMER_ARCHIVE_TRAILER:
			if (mystreamer->is_recovery_guc_supported)
			{
				/*
				 * If we didn't already find (and thus modify)
				 * postgresql.auto.conf, inject it as an additional archive
				 * member now.
				 */
				if (!mystreamer->found_postgresql_auto_conf)
					astreamer_inject_file(mystreamer->base.bbs_next,
										  "postgresql.auto.conf",
										  mystreamer->recoveryconfcontents->data,
										  mystreamer->recoveryconfcontents->len);

				/* Inject empty standby.signal file. */
				astreamer_inject_file(mystreamer->base.bbs_next,
									  "standby.signal", "", 0);
			}
			else
			{
				/* Inject recovery.conf file with specified contents. */
				astreamer_inject_file(mystreamer->base.bbs_next,
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

	astreamer_content(mystreamer->base.bbs_next, &mystreamer->member,
					  data, len, context);
}

/*
 * End-of-stream processing for this astreamer.
 */
static void
astreamer_recovery_injector_finalize(astreamer *streamer)
{
	astreamer_finalize(streamer->bbs_next);
}

/*
 * Free memory associated with this astreamer.
 */
static void
astreamer_recovery_injector_free(astreamer *streamer)
{
	astreamer_free(streamer->bbs_next);
	pfree(streamer);
}

/*
 * Inject a member into the archive with specified contents.
 */
void
astreamer_inject_file(astreamer *streamer, char *pathname, char *data,
					  int len)
{
	astreamer_member member;

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
	 * astreamer will have to generate them using the data from 'member'.
	 */
	astreamer_content(streamer, &member, NULL, 0,
					  ASTREAMER_MEMBER_HEADER);
	astreamer_content(streamer, &member, data, len,
					  ASTREAMER_MEMBER_CONTENTS);
	astreamer_content(streamer, &member, NULL, 0,
					  ASTREAMER_MEMBER_TRAILER);
}
