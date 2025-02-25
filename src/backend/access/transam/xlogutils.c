/*-------------------------------------------------------------------------
 *
 * xlogutils.c
 *
 * PostgreSQL write-ahead log manager utility routines
 *
 * This file contains support routines that are used by XLOG replay functions.
 * None of this code is used during normal system operation.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xlogutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/timeline.h"
#include "access/xlogrecovery.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/rel.h"


/* GUC variable */
bool		ignore_invalid_pages = false;

/*
 * Are we doing recovery from XLOG?
 *
 * This is only ever true in the startup process; it should be read as meaning
 * "this process is replaying WAL records", rather than "the system is in
 * recovery mode".  It should be examined primarily by functions that need
 * to act differently when called from a WAL redo function (e.g., to skip WAL
 * logging).  To check whether the system is in recovery regardless of which
 * process you're running in, use RecoveryInProgress() but only after shared
 * memory startup and lock initialization.
 *
 * This is updated from xlog.c and xlogrecovery.c, but lives here because
 * it's mostly read by WAL redo functions.
 */
bool		InRecovery = false;

/* Are we in Hot Standby mode? Only valid in startup process, see xlogutils.h */
HotStandbyState standbyState = STANDBY_DISABLED;

/*
 * During XLOG replay, we may see XLOG records for incremental updates of
 * pages that no longer exist, because their relation was later dropped or
 * truncated.  (Note: this is only possible when full_page_writes = OFF,
 * since when it's ON, the first reference we see to a page should always
 * be a full-page rewrite not an incremental update.)  Rather than simply
 * ignoring such records, we make a note of the referenced page, and then
 * complain if we don't actually see a drop or truncate covering the page
 * later in replay.
 */
typedef struct xl_invalid_page_key
{
	RelFileLocator locator;		/* the relation */
	ForkNumber	forkno;			/* the fork number */
	BlockNumber blkno;			/* the page */
} xl_invalid_page_key;

typedef struct xl_invalid_page
{
	xl_invalid_page_key key;	/* hash key ... must be first */
	bool		present;		/* page existed but contained zeroes */
} xl_invalid_page;

static HTAB *invalid_page_tab = NULL;

static int	read_local_xlog_page_guts(XLogReaderState *state, XLogRecPtr targetPagePtr,
									  int reqLen, XLogRecPtr targetRecPtr,
									  char *cur_page, bool wait_for_wal);

/* Report a reference to an invalid page */
static void
report_invalid_page(int elevel, RelFileLocator locator, ForkNumber forkno,
					BlockNumber blkno, bool present)
{
	RelPathStr	path = relpathperm(locator, forkno);

	if (present)
		elog(elevel, "page %u of relation %s is uninitialized",
			 blkno, path.str);
	else
		elog(elevel, "page %u of relation %s does not exist",
			 blkno, path.str);
}

/* Log a reference to an invalid page */
static void
log_invalid_page(RelFileLocator locator, ForkNumber forkno, BlockNumber blkno,
				 bool present)
{
	xl_invalid_page_key key;
	xl_invalid_page *hentry;
	bool		found;

	/*
	 * Once recovery has reached a consistent state, the invalid-page table
	 * should be empty and remain so. If a reference to an invalid page is
	 * found after consistency is reached, PANIC immediately. This might seem
	 * aggressive, but it's better than letting the invalid reference linger
	 * in the hash table until the end of recovery and PANIC there, which
	 * might come only much later if this is a standby server.
	 */
	if (reachedConsistency)
	{
		report_invalid_page(WARNING, locator, forkno, blkno, present);
		elog(ignore_invalid_pages ? WARNING : PANIC,
			 "WAL contains references to invalid pages");
	}

	/*
	 * Log references to invalid pages at DEBUG1 level.  This allows some
	 * tracing of the cause (note the elog context mechanism will tell us
	 * something about the XLOG record that generated the reference).
	 */
	if (message_level_is_interesting(DEBUG1))
		report_invalid_page(DEBUG1, locator, forkno, blkno, present);

	if (invalid_page_tab == NULL)
	{
		/* create hash table when first needed */
		HASHCTL		ctl;

		ctl.keysize = sizeof(xl_invalid_page_key);
		ctl.entrysize = sizeof(xl_invalid_page);

		invalid_page_tab = hash_create("XLOG invalid-page table",
									   100,
									   &ctl,
									   HASH_ELEM | HASH_BLOBS);
	}

	/* we currently assume xl_invalid_page_key contains no padding */
	key.locator = locator;
	key.forkno = forkno;
	key.blkno = blkno;
	hentry = (xl_invalid_page *)
		hash_search(invalid_page_tab, &key, HASH_ENTER, &found);

	if (!found)
	{
		/* hash_search already filled in the key */
		hentry->present = present;
	}
	else
	{
		/* repeat reference ... leave "present" as it was */
	}
}

/* Forget any invalid pages >= minblkno, because they've been dropped */
static void
forget_invalid_pages(RelFileLocator locator, ForkNumber forkno,
					 BlockNumber minblkno)
{
	HASH_SEQ_STATUS status;
	xl_invalid_page *hentry;

	if (invalid_page_tab == NULL)
		return;					/* nothing to do */

	hash_seq_init(&status, invalid_page_tab);

	while ((hentry = (xl_invalid_page *) hash_seq_search(&status)) != NULL)
	{
		if (RelFileLocatorEquals(hentry->key.locator, locator) &&
			hentry->key.forkno == forkno &&
			hentry->key.blkno >= minblkno)
		{
			elog(DEBUG2, "page %u of relation %s has been dropped",
				 hentry->key.blkno,
				 relpathperm(hentry->key.locator, forkno).str);

			if (hash_search(invalid_page_tab,
							&hentry->key,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "hash table corrupted");
		}
	}
}

/* Forget any invalid pages in a whole database */
static void
forget_invalid_pages_db(Oid dbid)
{
	HASH_SEQ_STATUS status;
	xl_invalid_page *hentry;

	if (invalid_page_tab == NULL)
		return;					/* nothing to do */

	hash_seq_init(&status, invalid_page_tab);

	while ((hentry = (xl_invalid_page *) hash_seq_search(&status)) != NULL)
	{
		if (hentry->key.locator.dbOid == dbid)
		{
			elog(DEBUG2, "page %u of relation %s has been dropped",
				 hentry->key.blkno,
				 relpathperm(hentry->key.locator, hentry->key.forkno).str);

			if (hash_search(invalid_page_tab,
							&hentry->key,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "hash table corrupted");
		}
	}
}

/* Are there any unresolved references to invalid pages? */
bool
XLogHaveInvalidPages(void)
{
	if (invalid_page_tab != NULL &&
		hash_get_num_entries(invalid_page_tab) > 0)
		return true;
	return false;
}

/* Complain about any remaining invalid-page entries */
void
XLogCheckInvalidPages(void)
{
	HASH_SEQ_STATUS status;
	xl_invalid_page *hentry;
	bool		foundone = false;

	if (invalid_page_tab == NULL)
		return;					/* nothing to do */

	hash_seq_init(&status, invalid_page_tab);

	/*
	 * Our strategy is to emit WARNING messages for all remaining entries and
	 * only PANIC after we've dumped all the available info.
	 */
	while ((hentry = (xl_invalid_page *) hash_seq_search(&status)) != NULL)
	{
		report_invalid_page(WARNING, hentry->key.locator, hentry->key.forkno,
							hentry->key.blkno, hentry->present);
		foundone = true;
	}

	if (foundone)
		elog(ignore_invalid_pages ? WARNING : PANIC,
			 "WAL contains references to invalid pages");

	hash_destroy(invalid_page_tab);
	invalid_page_tab = NULL;
}


/*
 * XLogReadBufferForRedo
 *		Read a page during XLOG replay
 *
 * Reads a block referenced by a WAL record into shared buffer cache, and
 * determines what needs to be done to redo the changes to it.  If the WAL
 * record includes a full-page image of the page, it is restored.
 *
 * 'record.EndRecPtr' is compared to the page's LSN to determine if the record
 * has already been replayed.  'block_id' is the ID number the block was
 * registered with, when the WAL record was created.
 *
 * Returns one of the following:
 *
 *	BLK_NEEDS_REDO	- changes from the WAL record need to be applied
 *	BLK_DONE		- block doesn't need replaying
 *	BLK_RESTORED	- block was restored from a full-page image included in
 *					  the record
 *	BLK_NOTFOUND	- block was not found (because it was truncated away by
 *					  an operation later in the WAL stream)
 *
 * On return, the buffer is locked in exclusive-mode, and returned in *buf.
 * Note that the buffer is locked and returned even if it doesn't need
 * replaying.  (Getting the buffer lock is not really necessary during
 * single-process crash recovery, but some subroutines such as MarkBufferDirty
 * will complain if we don't have the lock.  In hot standby mode it's
 * definitely necessary.)
 *
 * Note: when a backup block is available in XLOG with the BKPIMAGE_APPLY flag
 * set, we restore it, even if the page in the database appears newer.  This
 * is to protect ourselves against database pages that were partially or
 * incorrectly written during a crash.  We assume that the XLOG data must be
 * good because it has passed a CRC check, while the database page might not
 * be.  This will force us to replay all subsequent modifications of the page
 * that appear in XLOG, rather than possibly ignoring them as already
 * applied, but that's not a huge drawback.
 */
XLogRedoAction
XLogReadBufferForRedo(XLogReaderState *record, uint8 block_id,
					  Buffer *buf)
{
	return XLogReadBufferForRedoExtended(record, block_id, RBM_NORMAL,
										 false, buf);
}

/*
 * Pin and lock a buffer referenced by a WAL record, for the purpose of
 * re-initializing it.
 */
Buffer
XLogInitBufferForRedo(XLogReaderState *record, uint8 block_id)
{
	Buffer		buf;

	XLogReadBufferForRedoExtended(record, block_id, RBM_ZERO_AND_LOCK, false,
								  &buf);
	return buf;
}

/*
 * XLogReadBufferForRedoExtended
 *		Like XLogReadBufferForRedo, but with extra options.
 *
 * In RBM_ZERO_* modes, if the page doesn't exist, the relation is extended
 * with all-zeroes pages up to the referenced block number.  In
 * RBM_ZERO_AND_LOCK and RBM_ZERO_AND_CLEANUP_LOCK modes, the return value
 * is always BLK_NEEDS_REDO.
 *
 * (The RBM_ZERO_AND_CLEANUP_LOCK mode is redundant with the get_cleanup_lock
 * parameter. Do not use an inconsistent combination!)
 *
 * If 'get_cleanup_lock' is true, a "cleanup lock" is acquired on the buffer
 * using LockBufferForCleanup(), instead of a regular exclusive lock.
 */
XLogRedoAction
XLogReadBufferForRedoExtended(XLogReaderState *record,
							  uint8 block_id,
							  ReadBufferMode mode, bool get_cleanup_lock,
							  Buffer *buf)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber blkno;
	Buffer		prefetch_buffer;
	Page		page;
	bool		zeromode;
	bool		willinit;

	if (!XLogRecGetBlockTagExtended(record, block_id, &rlocator, &forknum, &blkno,
									&prefetch_buffer))
	{
		/* Caller specified a bogus block_id */
		elog(PANIC, "failed to locate backup block with ID %d in WAL record",
			 block_id);
	}

	/*
	 * Make sure that if the block is marked with WILL_INIT, the caller is
	 * going to initialize it. And vice versa.
	 */
	zeromode = (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK);
	willinit = (XLogRecGetBlock(record, block_id)->flags & BKPBLOCK_WILL_INIT) != 0;
	if (willinit && !zeromode)
		elog(PANIC, "block with WILL_INIT flag in WAL record must be zeroed by redo routine");
	if (!willinit && zeromode)
		elog(PANIC, "block to be initialized in redo routine must be marked with WILL_INIT flag in the WAL record");

	/* If it has a full-page image and it should be restored, do it. */
	if (XLogRecBlockImageApply(record, block_id))
	{
		Assert(XLogRecHasBlockImage(record, block_id));
		*buf = XLogReadBufferExtended(rlocator, forknum, blkno,
									  get_cleanup_lock ? RBM_ZERO_AND_CLEANUP_LOCK : RBM_ZERO_AND_LOCK,
									  prefetch_buffer);
		page = BufferGetPage(*buf);
		if (!RestoreBlockImage(record, block_id, page))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg_internal("%s", record->errormsg_buf)));

		/*
		 * The page may be uninitialized. If so, we can't set the LSN because
		 * that would corrupt the page.
		 */
		if (!PageIsNew(page))
		{
			PageSetLSN(page, lsn);
		}

		MarkBufferDirty(*buf);

		/*
		 * At the end of crash recovery the init forks of unlogged relations
		 * are copied, without going through shared buffers. So we need to
		 * force the on-disk state of init forks to always be in sync with the
		 * state in shared buffers.
		 */
		if (forknum == INIT_FORKNUM)
			FlushOneBuffer(*buf);

		return BLK_RESTORED;
	}
	else
	{
		*buf = XLogReadBufferExtended(rlocator, forknum, blkno, mode, prefetch_buffer);
		if (BufferIsValid(*buf))
		{
			if (mode != RBM_ZERO_AND_LOCK && mode != RBM_ZERO_AND_CLEANUP_LOCK)
			{
				if (get_cleanup_lock)
					LockBufferForCleanup(*buf);
				else
					LockBuffer(*buf, BUFFER_LOCK_EXCLUSIVE);
			}
			if (lsn <= PageGetLSN(BufferGetPage(*buf)))
				return BLK_DONE;
			else
				return BLK_NEEDS_REDO;
		}
		else
			return BLK_NOTFOUND;
	}
}

/*
 * XLogReadBufferExtended
 *		Read a page during XLOG replay
 *
 * This is functionally comparable to ReadBufferExtended. There's some
 * differences in the behavior wrt. the "mode" argument:
 *
 * In RBM_NORMAL mode, if the page doesn't exist, or contains all-zeroes, we
 * return InvalidBuffer. In this case the caller should silently skip the
 * update on this page. (In this situation, we expect that the page was later
 * dropped or truncated. If we don't see evidence of that later in the WAL
 * sequence, we'll complain at the end of WAL replay.)
 *
 * In RBM_ZERO_* modes, if the page doesn't exist, the relation is extended
 * with all-zeroes pages up to the given block number.
 *
 * In RBM_NORMAL_NO_LOG mode, we return InvalidBuffer if the page doesn't
 * exist, and we don't check for all-zeroes.  Thus, no log entry is made
 * to imply that the page should be dropped or truncated later.
 *
 * Optionally, recent_buffer can be used to provide a hint about the location
 * of the page in the buffer pool; it does not have to be correct, but avoids
 * a buffer mapping table probe if it is.
 *
 * NB: A redo function should normally not call this directly. To get a page
 * to modify, use XLogReadBufferForRedoExtended instead. It is important that
 * all pages modified by a WAL record are registered in the WAL records, or
 * they will be invisible to tools that need to know which pages are modified.
 */
Buffer
XLogReadBufferExtended(RelFileLocator rlocator, ForkNumber forknum,
					   BlockNumber blkno, ReadBufferMode mode,
					   Buffer recent_buffer)
{
	BlockNumber lastblock;
	Buffer		buffer;
	SMgrRelation smgr;

	Assert(blkno != P_NEW);

	/* Do we have a clue where the buffer might be already? */
	if (BufferIsValid(recent_buffer) &&
		mode == RBM_NORMAL &&
		ReadRecentBuffer(rlocator, forknum, blkno, recent_buffer))
	{
		buffer = recent_buffer;
		goto recent_buffer_fast_path;
	}

	/* Open the relation at smgr level */
	smgr = smgropen(rlocator, INVALID_PROC_NUMBER);

	/*
	 * Create the target file if it doesn't already exist.  This lets us cope
	 * if the replay sequence contains writes to a relation that is later
	 * deleted.  (The original coding of this routine would instead suppress
	 * the writes, but that seems like it risks losing valuable data if the
	 * filesystem loses an inode during a crash.  Better to write the data
	 * until we are actually told to delete the file.)
	 */
	smgrcreate(smgr, forknum, true);

	lastblock = smgrnblocks(smgr, forknum);

	if (blkno < lastblock)
	{
		/* page exists in file */
		buffer = ReadBufferWithoutRelcache(rlocator, forknum, blkno,
										   mode, NULL, true);
	}
	else
	{
		/* hm, page doesn't exist in file */
		if (mode == RBM_NORMAL)
		{
			log_invalid_page(rlocator, forknum, blkno, false);
			return InvalidBuffer;
		}
		if (mode == RBM_NORMAL_NO_LOG)
			return InvalidBuffer;
		/* OK to extend the file */
		/* we do this in recovery only - no rel-extension lock needed */
		Assert(InRecovery);
		buffer = ExtendBufferedRelTo(BMR_SMGR(smgr, RELPERSISTENCE_PERMANENT),
									 forknum,
									 NULL,
									 EB_PERFORMING_RECOVERY |
									 EB_SKIP_EXTENSION_LOCK,
									 blkno + 1,
									 mode);
	}

recent_buffer_fast_path:
	if (mode == RBM_NORMAL)
	{
		/* check that page has been initialized */
		Page		page = (Page) BufferGetPage(buffer);

		/*
		 * We assume that PageIsNew is safe without a lock. During recovery,
		 * there should be no other backends that could modify the buffer at
		 * the same time.
		 */
		if (PageIsNew(page))
		{
			ReleaseBuffer(buffer);
			log_invalid_page(rlocator, forknum, blkno, true);
			return InvalidBuffer;
		}
	}

	return buffer;
}

/*
 * Struct actually returned by CreateFakeRelcacheEntry, though the declared
 * return type is Relation.
 */
typedef struct
{
	RelationData reldata;		/* Note: this must be first */
	FormData_pg_class pgc;
} FakeRelCacheEntryData;

typedef FakeRelCacheEntryData *FakeRelCacheEntry;

/*
 * Create a fake relation cache entry for a physical relation
 *
 * It's often convenient to use the same functions in XLOG replay as in the
 * main codepath, but those functions typically work with a relcache entry.
 * We don't have a working relation cache during XLOG replay, but this
 * function can be used to create a fake relcache entry instead. Only the
 * fields related to physical storage, like rd_rel, are initialized, so the
 * fake entry is only usable in low-level operations like ReadBuffer().
 *
 * This is also used for syncing WAL-skipped files.
 *
 * Caller must free the returned entry with FreeFakeRelcacheEntry().
 */
Relation
CreateFakeRelcacheEntry(RelFileLocator rlocator)
{
	FakeRelCacheEntry fakeentry;
	Relation	rel;

	/* Allocate the Relation struct and all related space in one block. */
	fakeentry = palloc0(sizeof(FakeRelCacheEntryData));
	rel = (Relation) fakeentry;

	rel->rd_rel = &fakeentry->pgc;
	rel->rd_locator = rlocator;

	/*
	 * We will never be working with temp rels during recovery or while
	 * syncing WAL-skipped files.
	 */
	rel->rd_backend = INVALID_PROC_NUMBER;

	/* It must be a permanent table here */
	rel->rd_rel->relpersistence = RELPERSISTENCE_PERMANENT;

	/* We don't know the name of the relation; use relfilenumber instead */
	sprintf(RelationGetRelationName(rel), "%u", rlocator.relNumber);

	/*
	 * We set up the lockRelId in case anything tries to lock the dummy
	 * relation.  Note that this is fairly bogus since relNumber may be
	 * different from the relation's OID.  It shouldn't really matter though.
	 * In recovery, we are running by ourselves and can't have any lock
	 * conflicts.  While syncing, we already hold AccessExclusiveLock.
	 */
	rel->rd_lockInfo.lockRelId.dbId = rlocator.dbOid;
	rel->rd_lockInfo.lockRelId.relId = rlocator.relNumber;

	/*
	 * Set up a non-pinned SMgrRelation reference, so that we don't need to
	 * worry about unpinning it on error.
	 */
	rel->rd_smgr = smgropen(rlocator, INVALID_PROC_NUMBER);

	return rel;
}

/*
 * Free a fake relation cache entry.
 */
void
FreeFakeRelcacheEntry(Relation fakerel)
{
	pfree(fakerel);
}

/*
 * Drop a relation during XLOG replay
 *
 * This is called when the relation is about to be deleted; we need to remove
 * any open "invalid-page" records for the relation.
 */
void
XLogDropRelation(RelFileLocator rlocator, ForkNumber forknum)
{
	forget_invalid_pages(rlocator, forknum, 0);
}

/*
 * Drop a whole database during XLOG replay
 *
 * As above, but for DROP DATABASE instead of dropping a single rel
 */
void
XLogDropDatabase(Oid dbid)
{
	/*
	 * This is unnecessarily heavy-handed, as it will close SMgrRelation
	 * objects for other databases as well. DROP DATABASE occurs seldom enough
	 * that it's not worth introducing a variant of smgrdestroy for just this
	 * purpose.
	 */
	smgrdestroyall();

	forget_invalid_pages_db(dbid);
}

/*
 * Truncate a relation during XLOG replay
 *
 * We need to clean up any open "invalid-page" records for the dropped pages.
 */
void
XLogTruncateRelation(RelFileLocator rlocator, ForkNumber forkNum,
					 BlockNumber nblocks)
{
	forget_invalid_pages(rlocator, forkNum, nblocks);
}

/*
 * Determine which timeline to read an xlog page from and set the
 * XLogReaderState's currTLI to that timeline ID.
 *
 * We care about timelines in xlogreader when we might be reading xlog
 * generated prior to a promotion, either if we're currently a standby in
 * recovery or if we're a promoted primary reading xlogs generated by the old
 * primary before our promotion.
 *
 * wantPage must be set to the start address of the page to read and
 * wantLength to the amount of the page that will be read, up to
 * XLOG_BLCKSZ. If the amount to be read isn't known, pass XLOG_BLCKSZ.
 *
 * The currTLI argument should be the system-wide current timeline.
 * Note that this may be different from state->currTLI, which is the timeline
 * from which the caller is currently reading previous xlog records.
 *
 * We switch to an xlog segment from the new timeline eagerly when on a
 * historical timeline, as soon as we reach the start of the xlog segment
 * containing the timeline switch.  The server copied the segment to the new
 * timeline so all the data up to the switch point is the same, but there's no
 * guarantee the old segment will still exist. It may have been deleted or
 * renamed with a .partial suffix so we can't necessarily keep reading from
 * the old TLI even though tliSwitchPoint says it's OK.
 *
 * We can't just check the timeline when we read a page on a different segment
 * to the last page. We could've received a timeline switch from a cascading
 * upstream, so the current segment ends abruptly (possibly getting renamed to
 * .partial) and we have to switch to a new one.  Even in the middle of reading
 * a page we could have to dump the cached page and switch to a new TLI.
 *
 * Because of this, callers MAY NOT assume that currTLI is the timeline that
 * will be in a page's xlp_tli; the page may begin on an older timeline or we
 * might be reading from historical timeline data on a segment that's been
 * copied to a new timeline.
 *
 * The caller must also make sure it doesn't read past the current replay
 * position (using GetXLogReplayRecPtr) if executing in recovery, so it
 * doesn't fail to notice that the current timeline became historical.
 */
void
XLogReadDetermineTimeline(XLogReaderState *state, XLogRecPtr wantPage,
						  uint32 wantLength, TimeLineID currTLI)
{
	const XLogRecPtr lastReadPage = (state->seg.ws_segno *
									 state->segcxt.ws_segsize + state->segoff);

	Assert(wantPage != InvalidXLogRecPtr && wantPage % XLOG_BLCKSZ == 0);
	Assert(wantLength <= XLOG_BLCKSZ);
	Assert(state->readLen == 0 || state->readLen <= XLOG_BLCKSZ);
	Assert(currTLI != 0);

	/*
	 * If the desired page is currently read in and valid, we have nothing to
	 * do.
	 *
	 * The caller should've ensured that it didn't previously advance readOff
	 * past the valid limit of this timeline, so it doesn't matter if the
	 * current TLI has since become historical.
	 */
	if (lastReadPage == wantPage &&
		state->readLen != 0 &&
		lastReadPage + state->readLen >= wantPage + Min(wantLength, XLOG_BLCKSZ - 1))
		return;

	/*
	 * If we're reading from the current timeline, it hasn't become historical
	 * and the page we're reading is after the last page read, we can again
	 * just carry on. (Seeking backwards requires a check to make sure the
	 * older page isn't on a prior timeline).
	 *
	 * currTLI might've become historical since the caller obtained the value,
	 * but the caller is required not to read past the flush limit it saw at
	 * the time it looked up the timeline. There's nothing we can do about it
	 * if StartupXLOG() renames it to .partial concurrently.
	 */
	if (state->currTLI == currTLI && wantPage >= lastReadPage)
	{
		Assert(state->currTLIValidUntil == InvalidXLogRecPtr);
		return;
	}

	/*
	 * If we're just reading pages from a previously validated historical
	 * timeline and the timeline we're reading from is valid until the end of
	 * the current segment we can just keep reading.
	 */
	if (state->currTLIValidUntil != InvalidXLogRecPtr &&
		state->currTLI != currTLI &&
		state->currTLI != 0 &&
		((wantPage + wantLength) / state->segcxt.ws_segsize) <
		(state->currTLIValidUntil / state->segcxt.ws_segsize))
		return;

	/*
	 * If we reach this point we're either looking up a page for random
	 * access, the current timeline just became historical, or we're reading
	 * from a new segment containing a timeline switch. In all cases we need
	 * to determine the newest timeline on the segment.
	 *
	 * If it's the current timeline we can just keep reading from here unless
	 * we detect a timeline switch that makes the current timeline historical.
	 * If it's a historical timeline we can read all the segment on the newest
	 * timeline because it contains all the old timelines' data too. So only
	 * one switch check is required.
	 */
	{
		/*
		 * We need to re-read the timeline history in case it's been changed
		 * by a promotion or replay from a cascaded replica.
		 */
		List	   *timelineHistory = readTimeLineHistory(currTLI);
		XLogRecPtr	endOfSegment;

		endOfSegment = ((wantPage / state->segcxt.ws_segsize) + 1) *
			state->segcxt.ws_segsize - 1;
		Assert(wantPage / state->segcxt.ws_segsize ==
			   endOfSegment / state->segcxt.ws_segsize);

		/*
		 * Find the timeline of the last LSN on the segment containing
		 * wantPage.
		 */
		state->currTLI = tliOfPointInHistory(endOfSegment, timelineHistory);
		state->currTLIValidUntil = tliSwitchPoint(state->currTLI, timelineHistory,
												  &state->nextTLI);

		Assert(state->currTLIValidUntil == InvalidXLogRecPtr ||
			   wantPage + wantLength < state->currTLIValidUntil);

		list_free_deep(timelineHistory);

		elog(DEBUG3, "switched to timeline %u valid until %X/%X",
			 state->currTLI,
			 LSN_FORMAT_ARGS(state->currTLIValidUntil));
	}
}

/* XLogReaderRoutine->segment_open callback for local pg_wal files */
void
wal_segment_open(XLogReaderState *state, XLogSegNo nextSegNo,
				 TimeLineID *tli_p)
{
	TimeLineID	tli = *tli_p;
	char		path[MAXPGPATH];

	XLogFilePath(path, tli, nextSegNo, state->segcxt.ws_segsize);
	state->seg.ws_file = BasicOpenFile(path, O_RDONLY | PG_BINARY);
	if (state->seg.ws_file >= 0)
		return;

	if (errno == ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("requested WAL segment %s has already been removed",
						path)));
	else
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						path)));
}

/* stock XLogReaderRoutine->segment_close callback */
void
wal_segment_close(XLogReaderState *state)
{
	close(state->seg.ws_file);
	/* need to check errno? */
	state->seg.ws_file = -1;
}

/*
 * XLogReaderRoutine->page_read callback for reading local xlog files
 *
 * Public because it would likely be very helpful for someone writing another
 * output method outside walsender, e.g. in a bgworker.
 *
 * TODO: The walsender has its own version of this, but it relies on the
 * walsender's latch being set whenever WAL is flushed. No such infrastructure
 * exists for normal backends, so we have to do a check/sleep/repeat style of
 * loop for now.
 */
int
read_local_xlog_page(XLogReaderState *state, XLogRecPtr targetPagePtr,
					 int reqLen, XLogRecPtr targetRecPtr, char *cur_page)
{
	return read_local_xlog_page_guts(state, targetPagePtr, reqLen,
									 targetRecPtr, cur_page, true);
}

/*
 * Same as read_local_xlog_page except that it doesn't wait for future WAL
 * to be available.
 */
int
read_local_xlog_page_no_wait(XLogReaderState *state, XLogRecPtr targetPagePtr,
							 int reqLen, XLogRecPtr targetRecPtr,
							 char *cur_page)
{
	return read_local_xlog_page_guts(state, targetPagePtr, reqLen,
									 targetRecPtr, cur_page, false);
}

/*
 * Implementation of read_local_xlog_page and its no wait version.
 */
static int
read_local_xlog_page_guts(XLogReaderState *state, XLogRecPtr targetPagePtr,
						  int reqLen, XLogRecPtr targetRecPtr,
						  char *cur_page, bool wait_for_wal)
{
	XLogRecPtr	read_upto,
				loc;
	TimeLineID	tli;
	int			count;
	WALReadError errinfo;
	TimeLineID	currTLI;

	loc = targetPagePtr + reqLen;

	/* Loop waiting for xlog to be available if necessary */
	while (1)
	{
		/*
		 * Determine the limit of xlog we can currently read to, and what the
		 * most recent timeline is.
		 */
		if (!RecoveryInProgress())
			read_upto = GetFlushRecPtr(&currTLI);
		else
			read_upto = GetXLogReplayRecPtr(&currTLI);
		tli = currTLI;

		/*
		 * Check which timeline to get the record from.
		 *
		 * We have to do it each time through the loop because if we're in
		 * recovery as a cascading standby, the current timeline might've
		 * become historical. We can't rely on RecoveryInProgress() because in
		 * a standby configuration like
		 *
		 * A => B => C
		 *
		 * if we're a logical decoding session on C, and B gets promoted, our
		 * timeline will change while we remain in recovery.
		 *
		 * We can't just keep reading from the old timeline as the last WAL
		 * archive in the timeline will get renamed to .partial by
		 * StartupXLOG().
		 *
		 * If that happens after our caller determined the TLI but before we
		 * actually read the xlog page, we might still try to read from the
		 * old (now renamed) segment and fail. There's not much we can do
		 * about this, but it can only happen when we're a leaf of a cascading
		 * standby whose primary gets promoted while we're decoding, so a
		 * one-off ERROR isn't too bad.
		 */
		XLogReadDetermineTimeline(state, targetPagePtr, reqLen, tli);

		if (state->currTLI == currTLI)
		{

			if (loc <= read_upto)
				break;

			/* If asked, let's not wait for future WAL. */
			if (!wait_for_wal)
			{
				ReadLocalXLogPageNoWaitPrivate *private_data;

				/*
				 * Inform the caller of read_local_xlog_page_no_wait that the
				 * end of WAL has been reached.
				 */
				private_data = (ReadLocalXLogPageNoWaitPrivate *)
					state->private_data;
				private_data->end_of_wal = true;
				break;
			}

			CHECK_FOR_INTERRUPTS();
			pg_usleep(1000L);
		}
		else
		{
			/*
			 * We're on a historical timeline, so limit reading to the switch
			 * point where we moved to the next timeline.
			 *
			 * We don't need to GetFlushRecPtr or GetXLogReplayRecPtr. We know
			 * about the new timeline, so we must've received past the end of
			 * it.
			 */
			read_upto = state->currTLIValidUntil;

			/*
			 * Setting tli to our wanted record's TLI is slightly wrong; the
			 * page might begin on an older timeline if it contains a timeline
			 * switch, since its xlog segment will have been copied from the
			 * prior timeline. This is pretty harmless though, as nothing
			 * cares so long as the timeline doesn't go backwards.  We should
			 * read the page header instead; FIXME someday.
			 */
			tli = state->currTLI;

			/* No need to wait on a historical timeline */
			break;
		}
	}

	if (targetPagePtr + XLOG_BLCKSZ <= read_upto)
	{
		/*
		 * more than one block available; read only that block, have caller
		 * come back if they need more.
		 */
		count = XLOG_BLCKSZ;
	}
	else if (targetPagePtr + reqLen > read_upto)
	{
		/* not enough data there */
		return -1;
	}
	else
	{
		/* enough bytes available to satisfy the request */
		count = read_upto - targetPagePtr;
	}

	if (!WALRead(state, cur_page, targetPagePtr, count, tli,
				 &errinfo))
		WALReadRaiseError(&errinfo);

	/* number of valid bytes in the buffer */
	return count;
}

/*
 * Backend-specific convenience code to handle read errors encountered by
 * WALRead().
 */
void
WALReadRaiseError(WALReadError *errinfo)
{
	WALOpenSegment *seg = &errinfo->wre_seg;
	char		fname[MAXFNAMELEN];

	XLogFileName(fname, seg->ws_tli, seg->ws_segno, wal_segment_size);

	if (errinfo->wre_read < 0)
	{
		errno = errinfo->wre_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from WAL segment %s, offset %d: %m",
						fname, errinfo->wre_off)));
	}
	else if (errinfo->wre_read == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not read from WAL segment %s, offset %d: read %d of %d",
						fname, errinfo->wre_off, errinfo->wre_read,
						errinfo->wre_req)));
	}
}
