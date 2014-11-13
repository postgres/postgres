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
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
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
		ctl.hash = tag_hash;

		invalid_page_tab = hash_create("XLOG invalid-page table",
									   100,
									   &ctl,
									   HASH_ELEM | HASH_FUNCTION);
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
 * 'rnode' and 'blkno' point to the block being replayed (main fork number
 * is implied, use XLogReadBufferForRedoExtended for other forks).
 * 'block_index' identifies the backup block in the record for the page.
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
 */
XLogRedoAction
XLogReadBufferForRedo(XLogRecPtr lsn, XLogRecord *record, int block_index,
					  RelFileNode rnode, BlockNumber blkno,
					  Buffer *buf)
{
	return XLogReadBufferForRedoExtended(lsn, record, block_index,
										 rnode, MAIN_FORKNUM, blkno,
										 RBM_NORMAL, false, buf);
}

/*
 * XLogReadBufferForRedoExtended
 *		Like XLogReadBufferForRedo, but with extra options.
 *
 * If mode is RBM_ZERO or RBM_ZERO_ON_ERROR, if the page doesn't exist, the
 * relation is extended with all-zeroes pages up to the referenced block
 * number.  In RBM_ZERO mode, the return value is always BLK_NEEDS_REDO.
 *
 * If 'get_cleanup_lock' is true, a "cleanup lock" is acquired on the buffer
 * using LockBufferForCleanup(), instead of a regular exclusive lock.
 */
XLogRedoAction
XLogReadBufferForRedoExtended(XLogRecPtr lsn, XLogRecord *record,
							  int block_index, RelFileNode rnode,
							  ForkNumber forkno, BlockNumber blkno,
							  ReadBufferMode mode, bool get_cleanup_lock,
							  Buffer *buf)
{
	if (record->xl_info & XLR_BKP_BLOCK(block_index))
	{
		*buf = RestoreBackupBlock(lsn, record, block_index,
								  get_cleanup_lock, true);
		return BLK_RESTORED;
	}
	else
	{
		*buf = XLogReadBufferExtended(rnode, forkno, blkno, mode);
		if (BufferIsValid(*buf))
		{
			if (get_cleanup_lock)
				LockBufferForCleanup(*buf);
			else
				LockBuffer(*buf, BUFFER_LOCK_EXCLUSIVE);
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
 * XLogReadBuffer
 *		Read a page during XLOG replay.
 *
 * This is a shorthand of XLogReadBufferExtended() followed by
 * LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE), for reading from the main
 * fork.
 *
 * (Getting the buffer lock is not really necessary during single-process
 * crash recovery, but some subroutines such as MarkBufferDirty will complain
 * if we don't have the lock.  In hot standby mode it's definitely necessary.)
 *
 * The returned buffer is exclusively-locked.
 *
 * For historical reasons, instead of a ReadBufferMode argument, this only
 * supports RBM_ZERO (init == true) and RBM_NORMAL (init == false) modes.
 */
Buffer
XLogReadBuffer(RelFileNode rnode, BlockNumber blkno, bool init)
{
	Buffer		buf;

	buf = XLogReadBufferExtended(rnode, MAIN_FORKNUM, blkno,
								 init ? RBM_ZERO : RBM_NORMAL);
	if (BufferIsValid(buf))
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	return buf;
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
 * In RBM_ZERO and RBM_ZERO_ON_ERROR modes, if the page doesn't exist, the
 * relation is extended with all-zeroes pages up to the given block number.
 *
 * In RBM_NORMAL_NO_LOG mode, we return InvalidBuffer if the page doesn't
 * exist, and we don't check for all-zeroes.  Thus, no log entry is made
 * to imply that the page should be dropped or truncated later.
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
				ReleaseBuffer(buffer);
			buffer = ReadBufferWithoutRelcache(rnode, forknum,
											   P_NEW, mode, NULL);
		}
		while (BufferGetBlockNumber(buffer) < blkno);
		/* Handle the corner case that P_NEW returns non-consecutive pages */
		if (BufferGetBlockNumber(buffer) != blkno)
		{
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
 * Restore a full-page image from a backup block attached to an XLOG record.
 *
 * lsn: LSN of the XLOG record being replayed
 * record: the complete XLOG record
 * block_index: which backup block to restore (0 .. XLR_MAX_BKP_BLOCKS - 1)
 * get_cleanup_lock: TRUE to get a cleanup rather than plain exclusive lock
 * keep_buffer: TRUE to return the buffer still locked and pinned
 *
 * Returns the buffer number containing the page.  Note this is not terribly
 * useful unless keep_buffer is specified as TRUE.
 *
 * Note: when a backup block is available in XLOG, we restore it
 * unconditionally, even if the page in the database appears newer.
 * This is to protect ourselves against database pages that were partially
 * or incorrectly written during a crash.  We assume that the XLOG data
 * must be good because it has passed a CRC check, while the database
 * page might not be.  This will force us to replay all subsequent
 * modifications of the page that appear in XLOG, rather than possibly
 * ignoring them as already applied, but that's not a huge drawback.
 *
 * If 'get_cleanup_lock' is true, a cleanup lock is obtained on the buffer,
 * else a normal exclusive lock is used.  During crash recovery, that's just
 * pro forma because there can't be any regular backends in the system, but
 * in hot standby mode the distinction is important.
 *
 * If 'keep_buffer' is true, return without releasing the buffer lock and pin;
 * then caller is responsible for doing UnlockReleaseBuffer() later.  This
 * is needed in some cases when replaying XLOG records that touch multiple
 * pages, to prevent inconsistent states from being visible to other backends.
 * (Again, that's only important in hot standby mode.)
 */
Buffer
RestoreBackupBlock(XLogRecPtr lsn, XLogRecord *record, int block_index,
				   bool get_cleanup_lock, bool keep_buffer)
{
	BkpBlock	bkpb;
	char	   *blk;
	int			i;

	/* Locate requested BkpBlock in the record */
	blk = (char *) XLogRecGetData(record) + record->xl_len;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		if (!(record->xl_info & XLR_BKP_BLOCK(i)))
			continue;

		memcpy(&bkpb, blk, sizeof(BkpBlock));
		blk += sizeof(BkpBlock);

		if (i == block_index)
		{
			/* Found it, apply the update */
			return RestoreBackupBlockContents(lsn, bkpb, blk, get_cleanup_lock,
											  keep_buffer);
		}

		blk += BLCKSZ - bkpb.hole_length;
	}

	/* Caller specified a bogus block_index */
	elog(ERROR, "failed to restore block_index %d", block_index);
	return InvalidBuffer;		/* keep compiler quiet */
}

/*
 * Workhorse for RestoreBackupBlock usable without an xlog record
 *
 * Restores a full-page image from BkpBlock and a data pointer.
 */
Buffer
RestoreBackupBlockContents(XLogRecPtr lsn, BkpBlock bkpb, char *blk,
						   bool get_cleanup_lock, bool keep_buffer)
{
	Buffer		buffer;
	Page		page;

	buffer = XLogReadBufferExtended(bkpb.node, bkpb.fork, bkpb.block,
									RBM_ZERO);
	Assert(BufferIsValid(buffer));
	if (get_cleanup_lock)
		LockBufferForCleanup(buffer);
	else
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	page = (Page) BufferGetPage(buffer);

	if (bkpb.hole_length == 0)
	{
		memcpy((char *) page, blk, BLCKSZ);
	}
	else
	{
		memcpy((char *) page, blk, bkpb.hole_offset);
		/* must zero-fill the hole */
		MemSet((char *) page + bkpb.hole_offset, 0, bkpb.hole_length);
		memcpy((char *) page + (bkpb.hole_offset + bkpb.hole_length),
			   blk + bkpb.hole_offset,
			   BLCKSZ - (bkpb.hole_offset + bkpb.hole_length));
	}

	/*
	 * The checksum value on this page is currently invalid. We don't need to
	 * reset it here since it will be set before being written.
	 */

	/*
	 * The page may be uninitialized. If so, we can't set the LSN because that
	 * would corrupt the page.
	 */
	if (!PageIsNew(page))
	{
		PageSetLSN(page, lsn);
	}
	MarkBufferDirty(buffer);

	if (!keep_buffer)
		UnlockReleaseBuffer(buffer);

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
