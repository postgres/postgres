/*-------------------------------------------------------------------------
 *
 * bulk_write.c
 *	  Efficiently and reliably populate a new relation
 *
 * The assumption is that no other backends access the relation while we are
 * loading it, so we can take some shortcuts.  Pages already present in the
 * indicated fork when the bulk write operation is started are not modified
 * unless explicitly written to.  Do not mix operations through the regular
 * buffer manager and the bulk loading interface!
 *
 * We bypass the buffer manager to avoid the locking overhead, and call
 * smgrextend() directly.  A downside is that the pages will need to be
 * re-read into shared buffers on first use after the build finishes.  That's
 * usually a good tradeoff for large relations, and for small relations, the
 * overhead isn't very significant compared to creating the relation in the
 * first place.
 *
 * The pages are WAL-logged if needed.  To save on WAL header overhead, we
 * WAL-log several pages in one record.
 *
 * One tricky point is that because we bypass the buffer manager, we need to
 * register the relation for fsyncing at the next checkpoint ourselves, and
 * make sure that the relation is correctly fsync'd by us or the checkpointer
 * even if a checkpoint happens concurrently.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/bulk_write.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xloginsert.h"
#include "access/xlogrecord.h"
#include "storage/bufpage.h"
#include "storage/bulk_write.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/rel.h"

#define MAX_PENDING_WRITES XLR_MAX_BLOCK_ID

static const PGIOAlignedBlock zero_buffer = {{0}};	/* worth BLCKSZ */

typedef struct PendingWrite
{
	BulkWriteBuffer buf;
	BlockNumber blkno;
	bool		page_std;
} PendingWrite;

/*
 * Bulk writer state for one relation fork.
 */
struct BulkWriteState
{
	/* Information about the target relation we're writing */
	SMgrRelation smgr;
	ForkNumber	forknum;
	bool		use_wal;

	/* We keep several writes queued, and WAL-log them in batches */
	int			npending;
	PendingWrite pending_writes[MAX_PENDING_WRITES];

	/* Current size of the relation */
	BlockNumber relsize;

	/* The RedoRecPtr at the time that the bulk operation started */
	XLogRecPtr	start_RedoRecPtr;

	MemoryContext memcxt;
};

static void smgr_bulk_flush(BulkWriteState *bulkstate);

/*
 * Start a bulk write operation on a relation fork.
 */
BulkWriteState *
smgr_bulk_start_rel(Relation rel, ForkNumber forknum)
{
	return smgr_bulk_start_smgr(RelationGetSmgr(rel),
								forknum,
								RelationNeedsWAL(rel) || forknum == INIT_FORKNUM);
}

/*
 * Start a bulk write operation on a relation fork.
 *
 * This is like smgr_bulk_start_rel, but can be used without a relcache entry.
 */
BulkWriteState *
smgr_bulk_start_smgr(SMgrRelation smgr, ForkNumber forknum, bool use_wal)
{
	BulkWriteState *state;

	state = palloc(sizeof(BulkWriteState));
	state->smgr = smgr;
	state->forknum = forknum;
	state->use_wal = use_wal;

	state->npending = 0;
	state->relsize = smgrnblocks(smgr, forknum);

	state->start_RedoRecPtr = GetRedoRecPtr();

	/*
	 * Remember the memory context.  We will use it to allocate all the
	 * buffers later.
	 */
	state->memcxt = CurrentMemoryContext;

	return state;
}

/*
 * Finish bulk write operation.
 *
 * This WAL-logs and flushes any remaining pending writes to disk, and fsyncs
 * the relation if needed.
 */
void
smgr_bulk_finish(BulkWriteState *bulkstate)
{
	/* WAL-log and flush any remaining pages */
	smgr_bulk_flush(bulkstate);

	/*
	 * Fsync the relation, or register it for the next checkpoint, if
	 * necessary.
	 */
	if (SmgrIsTemp(bulkstate->smgr))
	{
		/* Temporary relations don't need to be fsync'd, ever */
	}
	else if (!bulkstate->use_wal)
	{
		/*----------
		 * This is either an unlogged relation, or a permanent relation but we
		 * skipped WAL-logging because wal_level=minimal:
		 *
		 * A) Unlogged relation
		 *
		 *    Unlogged relations will go away on crash, but they need to be
		 *    fsync'd on a clean shutdown. It's sufficient to call
		 *    smgrregistersync(), that ensures that the checkpointer will
		 *    flush it at the shutdown checkpoint. (It will flush it on the
		 *    next online checkpoint too, which is not strictly necessary.)
		 *
		 *    Note that the init-fork of an unlogged relation is not
		 *    considered unlogged for our purposes. It's treated like a
		 *    regular permanent relation. The callers will pass use_wal=true
		 *    for the init fork.
		 *
		 * B) Permanent relation, WAL-logging skipped because wal_level=minimal
		 *
		 *    This is a new relation, and we didn't WAL-log the pages as we
		 *    wrote, but they need to be fsync'd before commit.
		 *
		 *    We don't need to do that here, however. The fsync() is done at
		 *    commit, by smgrDoPendingSyncs() (*).
		 *
		 *    (*) smgrDoPendingSyncs() might decide to WAL-log the whole
		 *    relation at commit instead of fsyncing it, if the relation was
		 *    very small, but it's smgrDoPendingSyncs() responsibility in any
		 *    case.
		 *
		 * We cannot distinguish the two here, so conservatively assume it's
		 * an unlogged relation. A permanent relation with wal_level=minimal
		 * would require no actions, see above.
		 */
		smgrregistersync(bulkstate->smgr, bulkstate->forknum);
	}
	else
	{
		/*
		 * Permanent relation, WAL-logged normally.
		 *
		 * We already WAL-logged all the pages, so they will be replayed from
		 * WAL on crash. However, when we wrote out the pages, we passed
		 * skipFsync=true to avoid the overhead of registering all the writes
		 * with the checkpointer.  Register the whole relation now.
		 *
		 * There is one hole in that idea: If a checkpoint occurred while we
		 * were writing the pages, it already missed fsyncing the pages we had
		 * written before the checkpoint started.  A crash later on would
		 * replay the WAL starting from the checkpoint, therefore it wouldn't
		 * replay our earlier WAL records.  So if a checkpoint started after
		 * the bulk write, fsync the files now.
		 */

		/*
		 * Prevent a checkpoint from starting between the GetRedoRecPtr() and
		 * smgrregistersync() calls.
		 */
		Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
		MyProc->delayChkptFlags |= DELAY_CHKPT_START;

		if (bulkstate->start_RedoRecPtr != GetRedoRecPtr())
		{
			/*
			 * A checkpoint occurred and it didn't know about our writes, so
			 * fsync() the relation ourselves.
			 */
			MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
			smgrimmedsync(bulkstate->smgr, bulkstate->forknum);
			elog(DEBUG1, "flushed relation because a checkpoint occurred concurrently");
		}
		else
		{
			smgrregistersync(bulkstate->smgr, bulkstate->forknum);
			MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
		}
	}
}

static int
buffer_cmp(const void *a, const void *b)
{
	const PendingWrite *bufa = (const PendingWrite *) a;
	const PendingWrite *bufb = (const PendingWrite *) b;

	/* We should not see duplicated writes for the same block */
	Assert(bufa->blkno != bufb->blkno);
	if (bufa->blkno > bufb->blkno)
		return 1;
	else
		return -1;
}

/*
 * Finish all the pending writes.
 */
static void
smgr_bulk_flush(BulkWriteState *bulkstate)
{
	int			npending = bulkstate->npending;
	PendingWrite *pending_writes = bulkstate->pending_writes;

	if (npending == 0)
		return;

	if (npending > 1)
		qsort(pending_writes, npending, sizeof(PendingWrite), buffer_cmp);

	if (bulkstate->use_wal)
	{
		BlockNumber blknos[MAX_PENDING_WRITES];
		Page		pages[MAX_PENDING_WRITES];
		bool		page_std = true;

		for (int i = 0; i < npending; i++)
		{
			blknos[i] = pending_writes[i].blkno;
			pages[i] = pending_writes[i].buf->data;

			/*
			 * If any of the pages use !page_std, we log them all as such.
			 * That's a bit wasteful, but in practice, a mix of standard and
			 * non-standard page layout is rare.  None of the built-in AMs do
			 * that.
			 */
			if (!pending_writes[i].page_std)
				page_std = false;
		}
		log_newpages(&bulkstate->smgr->smgr_rlocator.locator, bulkstate->forknum,
					 npending, blknos, pages, page_std);
	}

	for (int i = 0; i < npending; i++)
	{
		BlockNumber blkno = pending_writes[i].blkno;
		Page		page = pending_writes[i].buf->data;

		PageSetChecksumInplace(page, blkno);

		if (blkno >= bulkstate->relsize)
		{
			/*
			 * If we have to write pages nonsequentially, fill in the space
			 * with zeroes until we come back and overwrite.  This is not
			 * logically necessary on standard Unix filesystems (unwritten
			 * space will read as zeroes anyway), but it should help to avoid
			 * fragmentation.  The dummy pages aren't WAL-logged though.
			 */
			while (blkno > bulkstate->relsize)
			{
				/* don't set checksum for all-zero page */
				smgrextend(bulkstate->smgr, bulkstate->forknum,
						   bulkstate->relsize,
						   &zero_buffer,
						   true);
				bulkstate->relsize++;
			}

			smgrextend(bulkstate->smgr, bulkstate->forknum, blkno, page, true);
			bulkstate->relsize++;
		}
		else
			smgrwrite(bulkstate->smgr, bulkstate->forknum, blkno, page, true);
		pfree(page);
	}

	bulkstate->npending = 0;
}

/*
 * Queue write of 'buf'.
 *
 * NB: this takes ownership of 'buf'!
 *
 * You are only allowed to write a given block once as part of one bulk write
 * operation.
 */
void
smgr_bulk_write(BulkWriteState *bulkstate, BlockNumber blocknum, BulkWriteBuffer buf, bool page_std)
{
	PendingWrite *w;

	w = &bulkstate->pending_writes[bulkstate->npending++];
	w->buf = buf;
	w->blkno = blocknum;
	w->page_std = page_std;

	if (bulkstate->npending == MAX_PENDING_WRITES)
		smgr_bulk_flush(bulkstate);
}

/*
 * Allocate a new buffer which can later be written with smgr_bulk_write().
 *
 * There is no function to free the buffer.  When you pass it to
 * smgr_bulk_write(), it takes ownership and frees it when it's no longer
 * needed.
 *
 * This is currently implemented as a simple palloc, but could be implemented
 * using a ring buffer or larger chunks in the future, so don't rely on it.
 */
BulkWriteBuffer
smgr_bulk_get_buf(BulkWriteState *bulkstate)
{
	return MemoryContextAllocAligned(bulkstate->memcxt, BLCKSZ, PG_IO_ALIGN_SIZE, 0);
}
