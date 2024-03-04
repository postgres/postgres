/*-------------------------------------------------------------------------
 *
 * basebackup_progress.c
 *	  Basebackup sink implementing progress tracking, including but not
 *	  limited to command progress reporting.
 *
 * This should be used even if the PROGRESS option to the replication
 * command BASE_BACKUP is not specified. Without that option, we won't
 * have tallied up the size of the files that are going to need to be
 * backed up, but we can still report to the command progress reporting
 * facility how much data we've processed.
 *
 * Moreover, we also use this as a convenient place to update certain
 * fields of the bbsink_state. That work is accurately described as
 * keeping track of our progress, but it's not just for introspection.
 * We need those fields to be updated properly in order for base backups
 * to work.
 *
 * This particular basebackup sink requires extra callbacks that most base
 * backup sinks don't. Rather than cramming those into the interface, we just
 * have a few extra functions here that basebackup.c can call. (We could put
 * the logic directly into that file as it's fairly simple, but it seems
 * cleaner to have everything related to progress reporting in one place.)
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_progress.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "backup/basebackup_sink.h"
#include "commands/progress.h"
#include "pgstat.h"

static void bbsink_progress_begin_backup(bbsink *sink);
static void bbsink_progress_archive_contents(bbsink *sink, size_t len);
static void bbsink_progress_end_archive(bbsink *sink);

static const bbsink_ops bbsink_progress_ops = {
	.begin_backup = bbsink_progress_begin_backup,
	.begin_archive = bbsink_forward_begin_archive,
	.archive_contents = bbsink_progress_archive_contents,
	.end_archive = bbsink_progress_end_archive,
	.begin_manifest = bbsink_forward_begin_manifest,
	.manifest_contents = bbsink_forward_manifest_contents,
	.end_manifest = bbsink_forward_end_manifest,
	.end_backup = bbsink_forward_end_backup,
	.cleanup = bbsink_forward_cleanup
};

/*
 * Create a new basebackup sink that performs progress tracking functions and
 * forwards data to a successor sink.
 */
bbsink *
bbsink_progress_new(bbsink *next, bool estimate_backup_size)
{
	bbsink	   *sink;

	Assert(next != NULL);

	sink = palloc0(sizeof(bbsink));
	*((const bbsink_ops **) &sink->bbs_ops) = &bbsink_progress_ops;
	sink->bbs_next = next;

	/*
	 * Report that a base backup is in progress, and set the total size of the
	 * backup to -1, which will get translated to NULL. If we're estimating
	 * the backup size, we'll insert the real estimate when we have it.
	 */
	pgstat_progress_start_command(PROGRESS_COMMAND_BASEBACKUP, InvalidOid);
	pgstat_progress_update_param(PROGRESS_BASEBACKUP_BACKUP_TOTAL, -1);

	return sink;
}

/*
 * Progress reporting at start of backup.
 */
static void
bbsink_progress_begin_backup(bbsink *sink)
{
	const int	index[] = {
		PROGRESS_BASEBACKUP_PHASE,
		PROGRESS_BASEBACKUP_BACKUP_TOTAL,
		PROGRESS_BASEBACKUP_TBLSPC_TOTAL
	};
	int64		val[3];

	/*
	 * Report that we are now streaming database files as a base backup. Also
	 * advertise the number of tablespaces, and, if known, the estimated total
	 * backup size.
	 */
	val[0] = PROGRESS_BASEBACKUP_PHASE_STREAM_BACKUP;
	if (sink->bbs_state->bytes_total_is_valid)
		val[1] = sink->bbs_state->bytes_total;
	else
		val[1] = -1;
	val[2] = list_length(sink->bbs_state->tablespaces);
	pgstat_progress_update_multi_param(3, index, val);

	/* Delegate to next sink. */
	bbsink_forward_begin_backup(sink);
}

/*
 * End-of archive progress reporting.
 */
static void
bbsink_progress_end_archive(bbsink *sink)
{
	/*
	 * We expect one archive per tablespace, so reaching the end of an archive
	 * also means reaching the end of a tablespace. (Some day we might have a
	 * reason to decouple these concepts.)
	 *
	 * If WAL is included in the backup, we'll mark the last tablespace
	 * complete before the last archive is complete, so we need a guard here
	 * to ensure that the number of tablespaces streamed doesn't exceed the
	 * total.
	 */
	if (sink->bbs_state->tablespace_num < list_length(sink->bbs_state->tablespaces))
		pgstat_progress_update_param(PROGRESS_BASEBACKUP_TBLSPC_STREAMED,
									 sink->bbs_state->tablespace_num + 1);

	/* Delegate to next sink. */
	bbsink_forward_end_archive(sink);

	/*
	 * This is a convenient place to update the bbsink_state's notion of which
	 * is the current tablespace. Note that the bbsink_state object is shared
	 * across all bbsink objects involved, but we're the outermost one and
	 * this is the very last thing we do.
	 */
	sink->bbs_state->tablespace_num++;
}

/*
 * Handle progress tracking for new archive contents.
 *
 * Increment the counter for the amount of data already streamed
 * by the given number of bytes, and update the progress report for
 * pg_stat_progress_basebackup.
 */
static void
bbsink_progress_archive_contents(bbsink *sink, size_t len)
{
	bbsink_state *state = sink->bbs_state;
	const int	index[] = {
		PROGRESS_BASEBACKUP_BACKUP_STREAMED,
		PROGRESS_BASEBACKUP_BACKUP_TOTAL
	};
	int64		val[2];
	int			nparam = 0;

	/* First update bbsink_state with # of bytes done. */
	state->bytes_done += len;

	/* Now forward to next sink. */
	bbsink_forward_archive_contents(sink, len);

	/* Prepare to set # of bytes done for command progress reporting. */
	val[nparam++] = state->bytes_done;

	/*
	 * We may also want to update # of total bytes, to avoid overflowing past
	 * 100% or the full size. This may make the total size number change as we
	 * approach the end of the backup (the estimate will always be wrong if
	 * WAL is included), but that's better than having the done column be
	 * bigger than the total.
	 */
	if (state->bytes_total_is_valid && state->bytes_done > state->bytes_total)
		val[nparam++] = state->bytes_done;

	pgstat_progress_update_multi_param(nparam, index, val);
}

/*
 * Advertise that we are waiting for the start-of-backup checkpoint.
 */
void
basebackup_progress_wait_checkpoint(void)
{
	pgstat_progress_update_param(PROGRESS_BASEBACKUP_PHASE,
								 PROGRESS_BASEBACKUP_PHASE_WAIT_CHECKPOINT);
}

/*
 * Advertise that we are estimating the backup size.
 */
void
basebackup_progress_estimate_backup_size(void)
{
	pgstat_progress_update_param(PROGRESS_BASEBACKUP_PHASE,
								 PROGRESS_BASEBACKUP_PHASE_ESTIMATE_BACKUP_SIZE);
}

/*
 * Advertise that we are waiting for WAL archiving at end-of-backup.
 */
void
basebackup_progress_wait_wal_archive(bbsink_state *state)
{
	const int	index[] = {
		PROGRESS_BASEBACKUP_PHASE,
		PROGRESS_BASEBACKUP_TBLSPC_STREAMED
	};
	int64		val[2];

	/*
	 * We report having finished all tablespaces at this point, even if the
	 * archive for the main tablespace is still open, because what's going to
	 * be added is WAL files, not files that are really from the main
	 * tablespace.
	 */
	val[0] = PROGRESS_BASEBACKUP_PHASE_WAIT_WAL_ARCHIVE;
	val[1] = list_length(state->tablespaces);
	pgstat_progress_update_multi_param(2, index, val);
}

/*
 * Advertise that we are transferring WAL files into the final archive.
 */
void
basebackup_progress_transfer_wal(void)
{
	pgstat_progress_update_param(PROGRESS_BASEBACKUP_PHASE,
								 PROGRESS_BASEBACKUP_PHASE_TRANSFER_WAL);
}

/*
 * Advertise that we are no longer performing a backup.
 */
void
basebackup_progress_done(void)
{
	pgstat_progress_end_command();
}
