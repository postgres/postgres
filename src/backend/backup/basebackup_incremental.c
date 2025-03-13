/*-------------------------------------------------------------------------
 *
 * basebackup_incremental.c
 *	  code for incremental backup support
 *
 * This code isn't actually in charge of taking an incremental backup;
 * the actual construction of the incremental backup happens in
 * basebackup.c. Here, we're concerned with providing the necessary
 * supports for that operation. In particular, we need to parse the
 * backup manifest supplied by the user taking the incremental backup
 * and extract the required information from it.
 *
 * Portions Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_incremental.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/timeline.h"
#include "access/xlog.h"
#include "backup/basebackup_incremental.h"
#include "backup/walsummary.h"
#include "common/blkreftable.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "common/parse_manifest.h"
#include "postmaster/walsummarizer.h"

#define	BLOCKS_PER_READ			512

/*
 * We expect to find the last lines of the manifest, including the checksum,
 * in the last MIN_CHUNK bytes of the manifest. We trigger an incremental
 * parse step if we are about to overflow MAX_CHUNK bytes.
 */
#define MIN_CHUNK  1024
#define MAX_CHUNK (128 *  1024)

/*
 * Details extracted from the WAL ranges present in the supplied backup manifest.
 */
typedef struct
{
	TimeLineID	tli;
	XLogRecPtr	start_lsn;
	XLogRecPtr	end_lsn;
} backup_wal_range;

/*
 * Details extracted from the file list present in the supplied backup manifest.
 */
typedef struct
{
	uint32		status;
	const char *path;
	uint64		size;
} backup_file_entry;

static uint32 hash_string_pointer(const char *s);
#define SH_PREFIX               backup_file
#define SH_ELEMENT_TYPE			backup_file_entry
#define SH_KEY_TYPE             const char *
#define SH_KEY                  path
#define SH_HASH_KEY(tb, key)    hash_string_pointer(key)
#define SH_EQUAL(tb, a, b)		(strcmp(a, b) == 0)
#define SH_SCOPE                static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

struct IncrementalBackupInfo
{
	/* Memory context for this object and its subsidiary objects. */
	MemoryContext mcxt;

	/* Temporary buffer for storing the manifest while parsing it. */
	StringInfoData buf;

	/* WAL ranges extracted from the backup manifest. */
	List	   *manifest_wal_ranges;

	/*
	 * Files extracted from the backup manifest.
	 *
	 * We don't really need this information, because we use WAL summaries to
	 * figure out what's changed. It would be unsafe to just rely on the list
	 * of files that existed before, because it's possible for a file to be
	 * removed and a new one created with the same name and different
	 * contents. In such cases, the whole file must still be sent. We can tell
	 * from the WAL summaries whether that happened, but not from the file
	 * list.
	 *
	 * Nonetheless, this data is useful for sanity checking. If a file that we
	 * think we shouldn't need to send is not present in the manifest for the
	 * prior backup, something has gone terribly wrong. We retain the file
	 * names and sizes, but not the checksums or last modified times, for
	 * which we have no use.
	 *
	 * One significant downside of storing this data is that it consumes
	 * memory. If that turns out to be a problem, we might have to decide not
	 * to retain this information, or to make it optional.
	 */
	backup_file_hash *manifest_files;

	/*
	 * Block-reference table for the incremental backup.
	 *
	 * It's possible that storing the entire block-reference table in memory
	 * will be a problem for some users. The in-memory format that we're using
	 * here is pretty efficient, converging to little more than 1 bit per
	 * block for relation forks with large numbers of modified blocks. It's
	 * possible, however, that if you try to perform an incremental backup of
	 * a database with a sufficiently large number of relations on a
	 * sufficiently small machine, you could run out of memory here. If that
	 * turns out to be a problem in practice, we'll need to be more clever.
	 */
	BlockRefTable *brtab;

	/*
	 * State object for incremental JSON parsing
	 */
	JsonManifestParseIncrementalState *inc_state;
};

static void manifest_process_version(JsonManifestParseContext *context,
									 int manifest_version);
static void manifest_process_system_identifier(JsonManifestParseContext *context,
											   uint64 manifest_system_identifier);
static void manifest_process_file(JsonManifestParseContext *context,
								  const char *pathname,
								  uint64 size,
								  pg_checksum_type checksum_type,
								  int checksum_length,
								  uint8 *checksum_payload);
static void manifest_process_wal_range(JsonManifestParseContext *context,
									   TimeLineID tli,
									   XLogRecPtr start_lsn,
									   XLogRecPtr end_lsn);
pg_noreturn static void manifest_report_error(JsonManifestParseContext *context,
											  const char *fmt,...)
			pg_attribute_printf(2, 3);
static int	compare_block_numbers(const void *a, const void *b);

/*
 * Create a new object for storing information extracted from the manifest
 * supplied when creating an incremental backup.
 */
IncrementalBackupInfo *
CreateIncrementalBackupInfo(MemoryContext mcxt)
{
	IncrementalBackupInfo *ib;
	MemoryContext oldcontext;
	JsonManifestParseContext *context;

	oldcontext = MemoryContextSwitchTo(mcxt);

	ib = palloc0(sizeof(IncrementalBackupInfo));
	ib->mcxt = mcxt;
	initStringInfo(&ib->buf);

	/*
	 * It's hard to guess how many files a "typical" installation will have in
	 * the data directory, but a fresh initdb creates almost 1000 files as of
	 * this writing, so it seems to make sense for our estimate to
	 * substantially higher.
	 */
	ib->manifest_files = backup_file_create(mcxt, 10000, NULL);

	context = palloc0(sizeof(JsonManifestParseContext));
	/* Parse the manifest. */
	context->private_data = ib;
	context->version_cb = manifest_process_version;
	context->system_identifier_cb = manifest_process_system_identifier;
	context->per_file_cb = manifest_process_file;
	context->per_wal_range_cb = manifest_process_wal_range;
	context->error_cb = manifest_report_error;

	ib->inc_state = json_parse_manifest_incremental_init(context);

	MemoryContextSwitchTo(oldcontext);

	return ib;
}

/*
 * Before taking an incremental backup, the caller must supply the backup
 * manifest from a prior backup. Each chunk of manifest data received
 * from the client should be passed to this function.
 */
void
AppendIncrementalManifestData(IncrementalBackupInfo *ib, const char *data,
							  int len)
{
	MemoryContext oldcontext;

	/* Switch to our memory context. */
	oldcontext = MemoryContextSwitchTo(ib->mcxt);

	if (ib->buf.len > MIN_CHUNK && ib->buf.len + len > MAX_CHUNK)
	{
		/*
		 * time for an incremental parse. We'll do all but the last MIN_CHUNK
		 * so that we have enough left for the final piece.
		 */
		json_parse_manifest_incremental_chunk(ib->inc_state, ib->buf.data,
											  ib->buf.len - MIN_CHUNK, false);
		/* now remove what we just parsed  */
		memmove(ib->buf.data, ib->buf.data + (ib->buf.len - MIN_CHUNK),
				MIN_CHUNK + 1);
		ib->buf.len = MIN_CHUNK;
	}

	appendBinaryStringInfo(&ib->buf, data, len);

	/* Switch back to previous memory context. */
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Finalize an IncrementalBackupInfo object after all manifest data has
 * been supplied via calls to AppendIncrementalManifestData.
 */
void
FinalizeIncrementalManifest(IncrementalBackupInfo *ib)
{
	MemoryContext oldcontext;

	/* Switch to our memory context. */
	oldcontext = MemoryContextSwitchTo(ib->mcxt);

	/* Parse the last chunk of the manifest */
	json_parse_manifest_incremental_chunk(ib->inc_state, ib->buf.data,
										  ib->buf.len, true);

	/* Done with the buffer, so release memory. */
	pfree(ib->buf.data);
	ib->buf.data = NULL;

	/* Done with inc_state, so release that memory too */
	json_parse_manifest_incremental_shutdown(ib->inc_state);

	/* Switch back to previous memory context. */
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Prepare to take an incremental backup.
 *
 * Before this function is called, AppendIncrementalManifestData and
 * FinalizeIncrementalManifest should have already been called to pass all
 * the manifest data to this object.
 *
 * This function performs sanity checks on the data extracted from the
 * manifest and figures out for which WAL ranges we need summaries, and
 * whether those summaries are available. Then, it reads and combines the
 * data from those summary files. It also updates the backup_state with the
 * reference TLI and LSN for the prior backup.
 */
void
PrepareForIncrementalBackup(IncrementalBackupInfo *ib,
							BackupState *backup_state)
{
	MemoryContext oldcontext;
	List	   *expectedTLEs;
	List	   *all_wslist,
			   *required_wslist = NIL;
	ListCell   *lc;
	TimeLineHistoryEntry **tlep;
	int			num_wal_ranges;
	int			i;
	bool		found_backup_start_tli = false;
	TimeLineID	earliest_wal_range_tli = 0;
	XLogRecPtr	earliest_wal_range_start_lsn = InvalidXLogRecPtr;
	TimeLineID	latest_wal_range_tli = 0;

	Assert(ib->buf.data == NULL);

	/* Switch to our memory context. */
	oldcontext = MemoryContextSwitchTo(ib->mcxt);

	/*
	 * A valid backup manifest must always contain at least one WAL range
	 * (usually exactly one, unless the backup spanned a timeline switch).
	 */
	num_wal_ranges = list_length(ib->manifest_wal_ranges);
	if (num_wal_ranges == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("manifest contains no required WAL ranges")));

	/*
	 * Match up the TLIs that appear in the WAL ranges of the backup manifest
	 * with those that appear in this server's timeline history. We expect
	 * every backup_wal_range to match to a TimeLineHistoryEntry; if it does
	 * not, that's an error.
	 *
	 * This loop also decides which of the WAL ranges is the manifest is most
	 * ancient and which one is the newest, according to the timeline history
	 * of this server, and stores TLIs of those WAL ranges into
	 * earliest_wal_range_tli and latest_wal_range_tli. It also updates
	 * earliest_wal_range_start_lsn to the start LSN of the WAL range for
	 * earliest_wal_range_tli.
	 *
	 * Note that the return value of readTimeLineHistory puts the latest
	 * timeline at the beginning of the list, not the end. Hence, the earliest
	 * TLI is the one that occurs nearest the end of the list returned by
	 * readTimeLineHistory, and the latest TLI is the one that occurs closest
	 * to the beginning.
	 */
	expectedTLEs = readTimeLineHistory(backup_state->starttli);
	tlep = palloc0(num_wal_ranges * sizeof(TimeLineHistoryEntry *));
	for (i = 0; i < num_wal_ranges; ++i)
	{
		backup_wal_range *range = list_nth(ib->manifest_wal_ranges, i);
		bool		saw_earliest_wal_range_tli = false;
		bool		saw_latest_wal_range_tli = false;

		/* Search this server's history for this WAL range's TLI. */
		foreach(lc, expectedTLEs)
		{
			TimeLineHistoryEntry *tle = lfirst(lc);

			if (tle->tli == range->tli)
			{
				tlep[i] = tle;
				break;
			}

			if (tle->tli == earliest_wal_range_tli)
				saw_earliest_wal_range_tli = true;
			if (tle->tli == latest_wal_range_tli)
				saw_latest_wal_range_tli = true;
		}

		/*
		 * An incremental backup can only be taken relative to a backup that
		 * represents a previous state of this server. If the backup requires
		 * WAL from a timeline that's not in our history, that definitely
		 * isn't the case.
		 */
		if (tlep[i] == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("timeline %u found in manifest, but not in this server's history",
							range->tli)));

		/*
		 * If we found this TLI in the server's history before encountering
		 * the latest TLI seen so far in the server's history, then this TLI
		 * is the latest one seen so far.
		 *
		 * If on the other hand we saw the earliest TLI seen so far before
		 * finding this TLI, this TLI is earlier than the earliest one seen so
		 * far. And if this is the first TLI for which we've searched, it's
		 * also the earliest one seen so far.
		 *
		 * On the first loop iteration, both things should necessarily be
		 * true.
		 */
		if (!saw_latest_wal_range_tli)
			latest_wal_range_tli = range->tli;
		if (earliest_wal_range_tli == 0 || saw_earliest_wal_range_tli)
		{
			earliest_wal_range_tli = range->tli;
			earliest_wal_range_start_lsn = range->start_lsn;
		}
	}

	/*
	 * Propagate information about the prior backup into the backup_label that
	 * will be generated for this backup.
	 */
	backup_state->istartpoint = earliest_wal_range_start_lsn;
	backup_state->istarttli = earliest_wal_range_tli;

	/*
	 * Sanity check start and end LSNs for the WAL ranges in the manifest.
	 *
	 * Commonly, there won't be any timeline switches during the prior backup
	 * at all, but if there are, they should happen at the same LSNs that this
	 * server switched timelines.
	 *
	 * Whether there are any timeline switches during the prior backup or not,
	 * the prior backup shouldn't require any WAL from a timeline prior to the
	 * start of that timeline. It also shouldn't require any WAL from later
	 * than the start of this backup.
	 *
	 * If any of these sanity checks fail, one possible explanation is that
	 * the user has generated WAL on the same timeline with the same LSNs more
	 * than once. For instance, if two standbys running on timeline 1 were
	 * both promoted and (due to a broken archiving setup) both selected new
	 * timeline ID 2, then it's possible that one of these checks might trip.
	 *
	 * Note that there are lots of ways for the user to do something very bad
	 * without tripping any of these checks, and they are not intended to be
	 * comprehensive. It's pretty hard to see how we could be certain of
	 * anything here. However, if there's a problem staring us right in the
	 * face, it's best to report it, so we do.
	 */
	for (i = 0; i < num_wal_ranges; ++i)
	{
		backup_wal_range *range = list_nth(ib->manifest_wal_ranges, i);

		if (range->tli == earliest_wal_range_tli)
		{
			if (range->start_lsn < tlep[i]->begin)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("manifest requires WAL from initial timeline %u starting at %X/%X, but that timeline begins at %X/%X",
								range->tli,
								LSN_FORMAT_ARGS(range->start_lsn),
								LSN_FORMAT_ARGS(tlep[i]->begin))));
		}
		else
		{
			if (range->start_lsn != tlep[i]->begin)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("manifest requires WAL from continuation timeline %u starting at %X/%X, but that timeline begins at %X/%X",
								range->tli,
								LSN_FORMAT_ARGS(range->start_lsn),
								LSN_FORMAT_ARGS(tlep[i]->begin))));
		}

		if (range->tli == latest_wal_range_tli)
		{
			if (range->end_lsn > backup_state->startpoint)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("manifest requires WAL from final timeline %u ending at %X/%X, but this backup starts at %X/%X",
								range->tli,
								LSN_FORMAT_ARGS(range->end_lsn),
								LSN_FORMAT_ARGS(backup_state->startpoint)),
						 errhint("This can happen for incremental backups on a standby if there was little activity since the previous backup.")));
		}
		else
		{
			if (range->end_lsn != tlep[i]->end)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("manifest requires WAL from non-final timeline %u ending at %X/%X, but this server switched timelines at %X/%X",
								range->tli,
								LSN_FORMAT_ARGS(range->end_lsn),
								LSN_FORMAT_ARGS(tlep[i]->end))));
		}

	}

	/*
	 * Wait for WAL summarization to catch up to the backup start LSN. This
	 * will throw an error if the WAL summarizer appears to be stuck. If WAL
	 * summarization gets disabled while we're waiting, this will return
	 * immediately, and we'll error out further down if the WAL summaries are
	 * incomplete.
	 */
	WaitForWalSummarization(backup_state->startpoint);

	/*
	 * Retrieve a list of all WAL summaries on any timeline that overlap with
	 * the LSN range of interest. We could instead call GetWalSummaries() once
	 * per timeline in the loop that follows, but that would involve reading
	 * the directory multiple times. It should be mildly faster - and perhaps
	 * a bit safer - to do it just once.
	 */
	all_wslist = GetWalSummaries(0, earliest_wal_range_start_lsn,
								 backup_state->startpoint);

	/*
	 * We need WAL summaries for everything that happened during the prior
	 * backup and everything that happened afterward up until the point where
	 * the current backup started.
	 */
	foreach(lc, expectedTLEs)
	{
		TimeLineHistoryEntry *tle = lfirst(lc);
		XLogRecPtr	tli_start_lsn = tle->begin;
		XLogRecPtr	tli_end_lsn = tle->end;
		XLogRecPtr	tli_missing_lsn = InvalidXLogRecPtr;
		List	   *tli_wslist;

		/*
		 * Working through the history of this server from the current
		 * timeline backwards, we skip everything until we find the timeline
		 * where this backup started. Most of the time, this means we won't
		 * skip anything at all, as it's unlikely that the timeline has
		 * changed since the beginning of the backup moments ago.
		 */
		if (tle->tli == backup_state->starttli)
		{
			found_backup_start_tli = true;
			tli_end_lsn = backup_state->startpoint;
		}
		else if (!found_backup_start_tli)
			continue;

		/*
		 * Find the summaries that overlap the LSN range of interest for this
		 * timeline. If this is the earliest timeline involved, the range of
		 * interest begins with the start LSN of the prior backup; otherwise,
		 * it begins at the LSN at which this timeline came into existence. If
		 * this is the latest TLI involved, the range of interest ends at the
		 * start LSN of the current backup; otherwise, it ends at the point
		 * where we switched from this timeline to the next one.
		 */
		if (tle->tli == earliest_wal_range_tli)
			tli_start_lsn = earliest_wal_range_start_lsn;
		tli_wslist = FilterWalSummaries(all_wslist, tle->tli,
										tli_start_lsn, tli_end_lsn);

		/*
		 * There is no guarantee that the WAL summaries we found cover the
		 * entire range of LSNs for which summaries are required, or indeed
		 * that we found any WAL summaries at all. Check whether we have a
		 * problem of that sort.
		 */
		if (!WalSummariesAreComplete(tli_wslist, tli_start_lsn, tli_end_lsn,
									 &tli_missing_lsn))
		{
			if (XLogRecPtrIsInvalid(tli_missing_lsn))
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("WAL summaries are required on timeline %u from %X/%X to %X/%X, but no summaries for that timeline and LSN range exist",
								tle->tli,
								LSN_FORMAT_ARGS(tli_start_lsn),
								LSN_FORMAT_ARGS(tli_end_lsn))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("WAL summaries are required on timeline %u from %X/%X to %X/%X, but the summaries for that timeline and LSN range are incomplete",
								tle->tli,
								LSN_FORMAT_ARGS(tli_start_lsn),
								LSN_FORMAT_ARGS(tli_end_lsn)),
						 errdetail("The first unsummarized LSN in this range is %X/%X.",
								   LSN_FORMAT_ARGS(tli_missing_lsn))));
		}

		/*
		 * Remember that we need to read these summaries.
		 *
		 * Technically, it's possible that this could read more files than
		 * required, since tli_wslist in theory could contain redundant
		 * summaries. For instance, if we have a summary from 0/10000000 to
		 * 0/20000000 and also one from 0/00000000 to 0/30000000, then the
		 * latter subsumes the former and the former could be ignored.
		 *
		 * We ignore this possibility because the WAL summarizer only tries to
		 * generate summaries that do not overlap. If somehow they exist,
		 * we'll do a bit of extra work but the results should still be
		 * correct.
		 */
		required_wslist = list_concat(required_wslist, tli_wslist);

		/*
		 * Timelines earlier than the one in which the prior backup began are
		 * not relevant.
		 */
		if (tle->tli == earliest_wal_range_tli)
			break;
	}

	/*
	 * Read all of the required block reference table files and merge all of
	 * the data into a single in-memory block reference table.
	 *
	 * See the comments for struct IncrementalBackupInfo for some thoughts on
	 * memory usage.
	 */
	ib->brtab = CreateEmptyBlockRefTable();
	foreach(lc, required_wslist)
	{
		WalSummaryFile *ws = lfirst(lc);
		WalSummaryIO wsio;
		BlockRefTableReader *reader;
		RelFileLocator rlocator;
		ForkNumber	forknum;
		BlockNumber limit_block;
		BlockNumber blocks[BLOCKS_PER_READ];

		wsio.file = OpenWalSummaryFile(ws, false);
		wsio.filepos = 0;
		ereport(DEBUG1,
				(errmsg_internal("reading WAL summary file \"%s\"",
								 FilePathName(wsio.file))));
		reader = CreateBlockRefTableReader(ReadWalSummary, &wsio,
										   FilePathName(wsio.file),
										   ReportWalSummaryError, NULL);
		while (BlockRefTableReaderNextRelation(reader, &rlocator, &forknum,
											   &limit_block))
		{
			BlockRefTableSetLimitBlock(ib->brtab, &rlocator,
									   forknum, limit_block);

			while (1)
			{
				unsigned	nblocks;
				unsigned	i;

				nblocks = BlockRefTableReaderGetBlocks(reader, blocks,
													   BLOCKS_PER_READ);
				if (nblocks == 0)
					break;

				for (i = 0; i < nblocks; ++i)
					BlockRefTableMarkBlockModified(ib->brtab, &rlocator,
												   forknum, blocks[i]);
			}
		}
		DestroyBlockRefTableReader(reader);
		FileClose(wsio.file);
	}

	/* Switch back to previous memory context. */
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Get the pathname that should be used when a file is sent incrementally.
 *
 * The result is a palloc'd string.
 */
char *
GetIncrementalFilePath(Oid dboid, Oid spcoid, RelFileNumber relfilenumber,
					   ForkNumber forknum, unsigned segno)
{
	RelPathStr	path;
	char	   *lastslash;
	char	   *ipath;

	path = GetRelationPath(dboid, spcoid, relfilenumber, INVALID_PROC_NUMBER,
						   forknum);

	lastslash = strrchr(path.str, '/');
	Assert(lastslash != NULL);
	*lastslash = '\0';

	if (segno > 0)
		ipath = psprintf("%s/INCREMENTAL.%s.%u", path.str, lastslash + 1, segno);
	else
		ipath = psprintf("%s/INCREMENTAL.%s", path.str, lastslash + 1);

	return ipath;
}

/*
 * How should we back up a particular file as part of an incremental backup?
 *
 * If the return value is BACK_UP_FILE_FULLY, caller should back up the whole
 * file just as if this were not an incremental backup.  The contents of the
 * relative_block_numbers array are unspecified in this case.
 *
 * If the return value is BACK_UP_FILE_INCREMENTALLY, caller should include
 * an incremental file in the backup instead of the entire file. On return,
 * *num_blocks_required will be set to the number of blocks that need to be
 * sent, and the actual block numbers will have been stored in
 * relative_block_numbers, which should be an array of at least RELSEG_SIZE.
 * In addition, *truncation_block_length will be set to the value that should
 * be included in the incremental file.
 */
FileBackupMethod
GetFileBackupMethod(IncrementalBackupInfo *ib, const char *path,
					Oid dboid, Oid spcoid,
					RelFileNumber relfilenumber, ForkNumber forknum,
					unsigned segno, size_t size,
					unsigned *num_blocks_required,
					BlockNumber *relative_block_numbers,
					unsigned *truncation_block_length)
{
	BlockNumber limit_block;
	BlockNumber start_blkno;
	BlockNumber stop_blkno;
	RelFileLocator rlocator;
	BlockRefTableEntry *brtentry;
	unsigned	i;
	unsigned	nblocks;

	/* Should only be called after PrepareForIncrementalBackup. */
	Assert(ib->buf.data == NULL);

	/*
	 * dboid could be InvalidOid if shared rel, but spcoid and relfilenumber
	 * should have legal values.
	 */
	Assert(OidIsValid(spcoid));
	Assert(RelFileNumberIsValid(relfilenumber));

	/*
	 * If the file size is too large or not a multiple of BLCKSZ, then
	 * something weird is happening, so give up and send the whole file.
	 */
	if ((size % BLCKSZ) != 0 || size / BLCKSZ > RELSEG_SIZE)
		return BACK_UP_FILE_FULLY;

	/*
	 * The free-space map fork is not properly WAL-logged, so we need to
	 * backup the entire file every time.
	 */
	if (forknum == FSM_FORKNUM)
		return BACK_UP_FILE_FULLY;

	/*
	 * If this file was not part of the prior backup, back it up fully.
	 *
	 * If this file was created after the prior backup and before the start of
	 * the current backup, then the WAL summary information will tell us to
	 * back up the whole file. However, if this file was created after the
	 * start of the current backup, then the WAL summary won't know anything
	 * about it. Without this logic, we would erroneously conclude that it was
	 * OK to send it incrementally.
	 *
	 * Note that the file could have existed at the time of the prior backup,
	 * gotten deleted, and then a new file with the same name could have been
	 * created.  In that case, this logic won't prevent the file from being
	 * backed up incrementally. But, if the deletion happened before the start
	 * of the current backup, the limit block will be 0, inducing a full
	 * backup. If the deletion happened after the start of the current backup,
	 * reconstruction will erroneously combine blocks from the current
	 * lifespan of the file with blocks from the previous lifespan -- but in
	 * this type of case, WAL replay to reach backup consistency should remove
	 * and recreate the file anyway, so the initial bogus contents should not
	 * matter.
	 */
	if (backup_file_lookup(ib->manifest_files, path) == NULL)
	{
		char	   *ipath;

		ipath = GetIncrementalFilePath(dboid, spcoid, relfilenumber,
									   forknum, segno);
		if (backup_file_lookup(ib->manifest_files, ipath) == NULL)
			return BACK_UP_FILE_FULLY;
	}

	/*
	 * Look up the special block reference table entry for the database as a
	 * whole.
	 */
	rlocator.spcOid = spcoid;
	rlocator.dbOid = dboid;
	rlocator.relNumber = 0;
	if (BlockRefTableGetEntry(ib->brtab, &rlocator, MAIN_FORKNUM,
							  &limit_block) != NULL)
	{
		/*
		 * According to the WAL summary, this database OID/tablespace OID
		 * pairing has been created since the previous backup. So, everything
		 * in it must be backed up fully.
		 */
		return BACK_UP_FILE_FULLY;
	}

	/* Look up the block reference table entry for this relfilenode. */
	rlocator.relNumber = relfilenumber;
	brtentry = BlockRefTableGetEntry(ib->brtab, &rlocator, forknum,
									 &limit_block);

	/*
	 * If there is no entry, then there have been no WAL-logged changes to the
	 * relation since the predecessor backup was taken, so we can back it up
	 * incrementally and need not include any modified blocks.
	 *
	 * However, if the file is zero-length, we should do a full backup,
	 * because an incremental file is always more than zero length, and it's
	 * silly to take an incremental backup when a full backup would be
	 * smaller.
	 */
	if (brtentry == NULL)
	{
		if (size == 0)
			return BACK_UP_FILE_FULLY;
		*num_blocks_required = 0;
		*truncation_block_length = size / BLCKSZ;
		return BACK_UP_FILE_INCREMENTALLY;
	}

	/*
	 * If the limit_block is less than or equal to the point where this
	 * segment starts, send the whole file.
	 */
	if (limit_block <= segno * RELSEG_SIZE)
		return BACK_UP_FILE_FULLY;

	/*
	 * Get relevant entries from the block reference table entry.
	 *
	 * We shouldn't overflow computing the start or stop block numbers, but if
	 * it manages to happen somehow, detect it and throw an error.
	 */
	start_blkno = segno * RELSEG_SIZE;
	stop_blkno = start_blkno + (size / BLCKSZ);
	if (start_blkno / RELSEG_SIZE != segno || stop_blkno < start_blkno)
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg_internal("overflow computing block number bounds for segment %u with size %zu",
								segno, size));

	/*
	 * This will write *absolute* block numbers into the output array, but
	 * we'll transpose them below.
	 */
	nblocks = BlockRefTableEntryGetBlocks(brtentry, start_blkno, stop_blkno,
										  relative_block_numbers, RELSEG_SIZE);
	Assert(nblocks <= RELSEG_SIZE);

	/*
	 * If we're going to have to send nearly all of the blocks, then just send
	 * the whole file, because that won't require much extra storage or
	 * transfer and will speed up and simplify backup restoration. It's not
	 * clear what threshold is most appropriate here and perhaps it ought to
	 * be configurable, but for now we're just going to say that if we'd need
	 * to send 90% of the blocks anyway, give up and send the whole file.
	 *
	 * NB: If you change the threshold here, at least make sure to back up the
	 * file fully when every single block must be sent, because there's
	 * nothing good about sending an incremental file in that case.
	 */
	if (nblocks * BLCKSZ > size * 0.9)
		return BACK_UP_FILE_FULLY;

	/*
	 * Looks like we can send an incremental file, so sort the block numbers
	 * and then transpose them from absolute block numbers to relative block
	 * numbers if necessary.
	 *
	 * NB: If the block reference table was using the bitmap representation
	 * for a given chunk, the block numbers in that chunk will already be
	 * sorted, but when the array-of-offsets representation is used, we can
	 * receive block numbers here out of order.
	 */
	qsort(relative_block_numbers, nblocks, sizeof(BlockNumber),
		  compare_block_numbers);
	if (start_blkno != 0)
	{
		for (i = 0; i < nblocks; ++i)
			relative_block_numbers[i] -= start_blkno;
	}
	*num_blocks_required = nblocks;

	/*
	 * The truncation block length is the minimum length of the reconstructed
	 * file. Any block numbers below this threshold that are not present in
	 * the backup need to be fetched from the prior backup. At or above this
	 * threshold, blocks should only be included in the result if they are
	 * present in the backup. (This may require inserting zero blocks if the
	 * blocks included in the backup are non-consecutive.)
	 */
	*truncation_block_length = size / BLCKSZ;
	if (BlockNumberIsValid(limit_block))
	{
		unsigned	relative_limit = limit_block - segno * RELSEG_SIZE;

		if (*truncation_block_length < relative_limit)
			*truncation_block_length = relative_limit;
	}

	/* Send it incrementally. */
	return BACK_UP_FILE_INCREMENTALLY;
}

/*
 * Compute the size for a header of an incremental file containing a given
 * number of blocks. The header is rounded to a multiple of BLCKSZ, but
 * only if the file will store some block data.
 */
size_t
GetIncrementalHeaderSize(unsigned num_blocks_required)
{
	size_t		result;

	/* Make sure we're not going to overflow. */
	Assert(num_blocks_required <= RELSEG_SIZE);

	/*
	 * Three four byte quantities (magic number, truncation block length,
	 * block count) followed by block numbers.
	 */
	result = 3 * sizeof(uint32) + (sizeof(BlockNumber) * num_blocks_required);

	/*
	 * Round the header size to a multiple of BLCKSZ - when not a multiple of
	 * BLCKSZ, add the missing fraction of a block. But do this only if the
	 * file will store data for some blocks, otherwise keep it small.
	 */
	if ((num_blocks_required > 0) && (result % BLCKSZ != 0))
		result += BLCKSZ - (result % BLCKSZ);

	return result;
}

/*
 * Compute the size for an incremental file containing a given number of blocks.
 */
size_t
GetIncrementalFileSize(unsigned num_blocks_required)
{
	size_t		result;

	/* Make sure we're not going to overflow. */
	Assert(num_blocks_required <= RELSEG_SIZE);

	/*
	 * Header with three four byte quantities (magic number, truncation block
	 * length, block count) followed by block numbers, rounded to a multiple
	 * of BLCKSZ (for files with block data), followed by block contents.
	 */
	result = GetIncrementalHeaderSize(num_blocks_required);
	result += BLCKSZ * num_blocks_required;

	return result;
}

/*
 * Helper function for filemap hash table.
 */
static uint32
hash_string_pointer(const char *s)
{
	unsigned char *ss = (unsigned char *) s;

	return hash_bytes(ss, strlen(s));
}

/*
 * This callback to validate the manifest version for incremental backup.
 */
static void
manifest_process_version(JsonManifestParseContext *context,
						 int manifest_version)
{
	/* Incremental backups don't work with manifest version 1 */
	if (manifest_version == 1)
		context->error_cb(context,
						  "backup manifest version 1 does not support incremental backup");
}

/*
 * This callback to validate the manifest system identifier against the current
 * database server.
 */
static void
manifest_process_system_identifier(JsonManifestParseContext *context,
								   uint64 manifest_system_identifier)
{
	uint64		system_identifier;

	/* Get system identifier of current system */
	system_identifier = GetSystemIdentifier();

	if (manifest_system_identifier != system_identifier)
		context->error_cb(context,
						  "system identifier in backup manifest is %llu, but database system identifier is %llu",
						  (unsigned long long) manifest_system_identifier,
						  (unsigned long long) system_identifier);
}

/*
 * This callback is invoked for each file mentioned in the backup manifest.
 *
 * We store the path to each file and the size of each file for sanity-checking
 * purposes. For further details, see comments for IncrementalBackupInfo.
 */
static void
manifest_process_file(JsonManifestParseContext *context,
					  const char *pathname, uint64 size,
					  pg_checksum_type checksum_type,
					  int checksum_length,
					  uint8 *checksum_payload)
{
	IncrementalBackupInfo *ib = context->private_data;
	backup_file_entry *entry;
	bool		found;

	entry = backup_file_insert(ib->manifest_files, pathname, &found);
	if (!found)
	{
		entry->path = MemoryContextStrdup(ib->manifest_files->ctx,
										  pathname);
		entry->size = size;
	}
}

/*
 * This callback is invoked for each WAL range mentioned in the backup
 * manifest.
 *
 * We're just interested in learning the oldest LSN and the corresponding TLI
 * that appear in any WAL range.
 */
static void
manifest_process_wal_range(JsonManifestParseContext *context,
						   TimeLineID tli, XLogRecPtr start_lsn,
						   XLogRecPtr end_lsn)
{
	IncrementalBackupInfo *ib = context->private_data;
	backup_wal_range *range = palloc(sizeof(backup_wal_range));

	range->tli = tli;
	range->start_lsn = start_lsn;
	range->end_lsn = end_lsn;
	ib->manifest_wal_ranges = lappend(ib->manifest_wal_ranges, range);
}

/*
 * This callback is invoked if an error occurs while parsing the backup
 * manifest.
 */
static void
manifest_report_error(JsonManifestParseContext *context, const char *fmt,...)
{
	StringInfoData errbuf;

	initStringInfo(&errbuf);

	for (;;)
	{
		va_list		ap;
		int			needed;

		va_start(ap, fmt);
		needed = appendStringInfoVA(&errbuf, fmt, ap);
		va_end(ap);
		if (needed == 0)
			break;
		enlargeStringInfo(&errbuf, needed);
	}

	ereport(ERROR,
			errmsg_internal("%s", errbuf.data));
}

/*
 * Quicksort comparator for block numbers.
 */
static int
compare_block_numbers(const void *a, const void *b)
{
	BlockNumber aa = *(BlockNumber *) a;
	BlockNumber bb = *(BlockNumber *) b;

	return pg_cmp_u32(aa, bb);
}
