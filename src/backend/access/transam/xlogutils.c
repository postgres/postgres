/*-------------------------------------------------------------------------
 *
 * xlogutils.c
 *
 * PostgreSQL transaction log manager utility routines
 *
 * This file contains support routines that are used by XLOG replay functions.
 * None of this code is used during normal system operation.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xlogutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/rel.h"


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
	RelFileNode node;			/* the relation */
	ForkNumber	forkno;			/* the fork number */
	BlockNumber blkno;			/* the page */
} xl_invalid_page_key;

typedef struct xl_invalid_page
{
	xl_invalid_page_key key;	/* hash key ... must be first */
	bool		present;		/* page existed but contained zeroes */
} xl_invalid_page;

static HTAB *invalid_page_tab = NULL;


/* Report a reference to an invalid page */
static void
report_invalid_page(int elevel, RelFileNode node, ForkNumber forkno,
					BlockNumber blkno, bool present)
{
	char	   *path = relpathperm(node, forkno);

	if (present)
		elog(elevel, "page %u of relation %s is uninitialized",
			 blkno, path);
	else
		elog(elevel, "page %u of relation %s does not exist",
			 blkno, path);
	pfree(path);
}

/* Log a reference to an invalid page */
static void
log_invalid_page(RelFileNode node, ForkNumber forkno, BlockNumber blkno,
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
		report_invalid_page(WARNING, node, forkno, blkno, present);
		elog(PANIC, "WAL contains references to invalid pages");
	}

	/*
	 * Log references to invalid pages at DEBUG1 level.  This allows some
	 * tracing of the cause (note the elog context mechanism will tell us
	 * something about the XLOG record that generated the reference).
	 */
	if (log_min_messages <= DEBUG1 || client_min_messages <= DEBUG1)
		report_invalid_page(DEBUG1, node, forkno, blkno, present);

	if (invalid_page_tab == NULL)
	{
		/* create hash table when first needed */
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(xl_invalid_page_key);
		ctl.entrysize = sizeof(xl_invalid_page);

		invalid_page_tab = hash_create("XLOG invalid-page table",
									   100,
									   &ctl,
									   HASH_ELEM | HASH_BLOBS);
	}

	/* we currently assume xl_invalid_page_key contains no padding */
	key.node = node;
	key.forkno = forkno;
	key.blkno = blkno;
	hentry = (xl_invalid_page *)
		hash_search(invalid_page_tab, (void *) &key, HASH_ENTER, &found);

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
forget_invalid_pages(RelFileNode node, ForkNumber forkno, BlockNumber minblkno)
{
	HASH_SEQ_STATUS status;
	xl_invalid_page *hentry;

	if (invalid_page_tab == NULL)
		return;					/* nothing to do */

	hash_seq_init(&status, invalid_page_tab);

	while ((hentry = (xl_invalid_page *) hash_seq_search(&status)) != NULL)
	{
		if (RelFileNodeEquals(hentry->key.node, node) &&
			hentry->key.forkno == forkno &&
			hentry->key.blkno >= minblkno)
		{
			if (log_min_messages <= DEBUG2 || client_min_messages <= DEBUG2)
			{
				char	   *path = relpathperm(hentry->key.node, forkno);

				elog(DEBUG2, "page %u of relation %s has been dropped",
					 hentry->key.blkno, path);
				pfree(path);
			}

			if (hash_search(invalid_page_tab,
							(void *) &hentry->key,
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
		if (hentry->key.node.dbNode == dbid)
		{
			if (log_min_messages <= DEBUG2 || client_min_messages <= DEBUG2)
			{
				char	   *path = relpathperm(hentry->key.node, hentry->key.forkno);

				elog(DEBUG2, "page %u of relation %s has been dropped",
					 hentry->key.blkno, path);
				pfree(path);
			}

			if (hash_search(invalid_page_tab,
							(void *) &hentry->key,
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
		report_invalid_page(WARNING, hentry->key.node, hentry->key.forkno,
							hentry->key.blkno, hentry->present);
		foundone = true;
	}

	if (foundone)
		elog(PANIC, "WAL contains references to invalid pages");

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
 * 'lsn' is the LSN of the record being replayed.  It is compared with the
 * page's LSN to determine if the record has already been replayed.
 * 'block_id' is the ID number the block was registered with, when the WAL
 * record was created.
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
 * Note: when a backup block is available in XLOG, we restore it
 * unconditionally, even if the page in the database appears newer.  This is
 * to protect ourselves against database pages that were partially or
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
	RelFileNode rnode;
	ForkNumber	forknum;
	BlockNumber blkno;
	Page		page;
	bool		zeromode;
	bool		willinit;

	if (!XLogRecGetBlockTag(record, block_id, &rnode, &forknum, &blkno))
	{
		/* Caller specified a bogus block_id */
		elog(PANIC, "failed to locate backup block with ID %d", block_id);
	}

	/*
	 * Make sure that if the block is marked with WILL_INIT, the caller is
	 * going to initialize it. And vice versa.
	 */
	zeromode = (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK);
	willinit = (record->blocks[block_id].flags & BKPBLOCK_WILL_INIT) != 0;
	if (willinit && !zeromode)
		elog(PANIC, "block with WILL_INIT flag in WAL record must be zeroed by redo routine");
	if (!willinit && zeromode)
		elog(PANIC, "block to be initialized in redo routine must be marked with WILL_INIT flag in the WAL record");

	/* If it's a full-page image, restore it. */
	if (XLogRecHasBlockImage(record, block_id))
	{
		*buf = XLogReadBufferExtended(rnode, forknum, blkno,
		   get_cleanup_lock ? RBM_ZERO_AND_CLEANUP_LOCK : RBM_ZERO_AND_LOCK);
		page = BufferGetPage(*buf);
		if (!RestoreBlockImage(record, block_id, page))
			elog(ERROR, "failed to restore block image");

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
		*buf = XLogReadBufferExtended(rnode, forknum, blkno, mode);
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
 * NB: A redo function should normally not call this directly. To get a page
 * to modify, use XLogReplayBuffer instead. It is important that all pages
 * modified by a WAL record are registered in the WAL records, or they will be
 * invisible to tools that that need to know which pages are modified.
 */
Buffer
XLogReadBufferExtended(RelFileNode rnode, ForkNumber forknum,
					   BlockNumber blkno, ReadBufferMode mode)
{
	BlockNumber lastblock;
	Buffer		buffer;
	SMgrRelation smgr;

	Assert(blkno != P_NEW);

	/* Open the relation at smgr level */
	smgr = smgropen(rnode, InvalidBackendId);

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
		buffer = ReadBufferWithoutRelcache(rnode, forknum, blkno,
										   mode, NULL);
	}
	else
	{
		/* hm, page doesn't exist in file */
		if (mode == RBM_NORMAL)
		{
			log_invalid_page(rnode, forknum, blkno, false);
			return InvalidBuffer;
		}
		if (mode == RBM_NORMAL_NO_LOG)
			return InvalidBuffer;
		/* OK to extend the file */
		/* we do this in recovery only - no rel-extension lock needed */
		Assert(InRecovery);
		buffer = InvalidBuffer;
		do
		{
			if (buffer != InvalidBuffer)
			{
				if (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK)
					LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(buffer);
			}
			buffer = ReadBufferWithoutRelcache(rnode, forknum,
											   P_NEW, mode, NULL);
		}
		while (BufferGetBlockNumber(buffer) < blkno);
		/* Handle the corner case that P_NEW returns non-consecutive pages */
		if (BufferGetBlockNumber(buffer) != blkno)
		{
			if (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK)
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
			buffer = ReadBufferWithoutRelcache(rnode, forknum, blkno,
											   mode, NULL);
		}
	}

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
			log_invalid_page(rnode, forknum, blkno, true);
			return InvalidBuffer;
		}
	}

	return buffer;
}

/*
 * Struct actually returned by XLogFakeRelcacheEntry, though the declared
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
 * Caller must free the returned entry with FreeFakeRelcacheEntry().
 */
Relation
CreateFakeRelcacheEntry(RelFileNode rnode)
{
	FakeRelCacheEntry fakeentry;
	Relation	rel;

	Assert(InRecovery);

	/* Allocate the Relation struct and all related space in one block. */
	fakeentry = palloc0(sizeof(FakeRelCacheEntryData));
	rel = (Relation) fakeentry;

	rel->rd_rel = &fakeentry->pgc;
	rel->rd_node = rnode;
	/* We will never be working with temp rels during recovery */
	rel->rd_backend = InvalidBackendId;

	/* It must be a permanent table if we're in recovery. */
	rel->rd_rel->relpersistence = RELPERSISTENCE_PERMANENT;

	/* We don't know the name of the relation; use relfilenode instead */
	sprintf(RelationGetRelationName(rel), "%u", rnode.relNode);

	/*
	 * We set up the lockRelId in case anything tries to lock the dummy
	 * relation.  Note that this is fairly bogus since relNode may be
	 * different from the relation's OID.  It shouldn't really matter though,
	 * since we are presumably running by ourselves and can't have any lock
	 * conflicts ...
	 */
	rel->rd_lockInfo.lockRelId.dbId = rnode.dbNode;
	rel->rd_lockInfo.lockRelId.relId = rnode.relNode;

	rel->rd_smgr = NULL;

	return rel;
}

/*
 * Free a fake relation cache entry.
 */
void
FreeFakeRelcacheEntry(Relation fakerel)
{
	/* make sure the fakerel is not referenced by the SmgrRelation anymore */
	if (fakerel->rd_smgr != NULL)
		smgrclearowner(&fakerel->rd_smgr, fakerel->rd_smgr);
	pfree(fakerel);
}

/*
 * Drop a relation during XLOG replay
 *
 * This is called when the relation is about to be deleted; we need to remove
 * any open "invalid-page" records for the relation.
 */
void
XLogDropRelation(RelFileNode rnode, ForkNumber forknum)
{
	forget_invalid_pages(rnode, forknum, 0);
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
	 * that it's not worth introducing a variant of smgrclose for just this
	 * purpose. XXX: Or should we rather leave the smgr entries dangling?
	 */
	smgrcloseall();

	forget_invalid_pages_db(dbid);
}

/*
 * Truncate a relation during XLOG replay
 *
 * We need to clean up any open "invalid-page" records for the dropped pages.
 */
void
XLogTruncateRelation(RelFileNode rnode, ForkNumber forkNum,
					 BlockNumber nblocks)
{
	forget_invalid_pages(rnode, forkNum, nblocks);
}
