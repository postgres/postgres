/*-------------------------------------------------------------------------
 *
 * storage.c
 *	  code to create and destroy physical storage for relations
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/storage.c,v 1.6.2.1 2010/02/08 20:00:55 heikki Exp $
 *
 * NOTES
 *	  Some of this code used to be in storage/smgr/smgr.c, and the
 *	  function names still reflect that.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/storage.h"
#include "storage/freespace.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * We keep a list of all relations (represented as RelFileNode values)
 * that have been created or deleted in the current transaction.  When
 * a relation is created, we create the physical file immediately, but
 * remember it so that we can delete the file again if the current
 * transaction is aborted.  Conversely, a deletion request is NOT
 * executed immediately, but is just entered in the list.  When and if
 * the transaction commits, we can delete the physical file.
 *
 * To handle subtransactions, every entry is marked with its transaction
 * nesting level.  At subtransaction commit, we reassign the subtransaction's
 * entries to the parent nesting level.  At subtransaction abort, we can
 * immediately execute the abort-time actions for all entries of the current
 * nesting level.
 *
 * NOTE: the list is kept in TopMemoryContext to be sure it won't disappear
 * unbetimes.  It'd probably be OK to keep it in TopTransactionContext,
 * but I'm being paranoid.
 */

typedef struct PendingRelDelete
{
	RelFileNode relnode;		/* relation that may need to be deleted */
	bool		isTemp;			/* is it a temporary relation? */
	bool		atCommit;		/* T=delete at commit; F=delete at abort */
	int			nestLevel;		/* xact nesting level of request */
	struct PendingRelDelete *next;		/* linked-list link */
} PendingRelDelete;

static PendingRelDelete *pendingDeletes = NULL; /* head of linked list */

/*
 * Declarations for smgr-related XLOG records
 *
 * Note: we log file creation and truncation here, but logging of deletion
 * actions is handled by xact.c, because it is part of transaction commit.
 */

/* XLOG gives us high 4 bits */
#define XLOG_SMGR_CREATE	0x10
#define XLOG_SMGR_TRUNCATE	0x20

typedef struct xl_smgr_create
{
	RelFileNode rnode;
} xl_smgr_create;

typedef struct xl_smgr_truncate
{
	BlockNumber blkno;
	RelFileNode rnode;
} xl_smgr_truncate;


/*
 * RelationCreateStorage
 *		Create physical storage for a relation.
 *
 * Create the underlying disk file storage for the relation. This only
 * creates the main fork; additional forks are created lazily by the
 * modules that need them.
 *
 * This function is transactional. The creation is WAL-logged, and if the
 * transaction aborts later on, the storage will be destroyed.
 */
void
RelationCreateStorage(RelFileNode rnode, bool istemp)
{
	PendingRelDelete *pending;
	XLogRecPtr	lsn;
	XLogRecData rdata;
	xl_smgr_create xlrec;
	SMgrRelation srel;

	srel = smgropen(rnode);
	smgrcreate(srel, MAIN_FORKNUM, false);

	if (!istemp)
	{
		/*
		 * Make an XLOG entry showing the file creation.  If we abort, the
		 * file will be dropped at abort time.
		 */
		xlrec.rnode = rnode;

		rdata.data = (char *) &xlrec;
		rdata.len = sizeof(xlrec);
		rdata.buffer = InvalidBuffer;
		rdata.next = NULL;

		lsn = XLogInsert(RM_SMGR_ID, XLOG_SMGR_CREATE, &rdata);
	}

	/* Add the relation to the list of stuff to delete at abort */
	pending = (PendingRelDelete *)
		MemoryContextAlloc(TopMemoryContext, sizeof(PendingRelDelete));
	pending->relnode = rnode;
	pending->isTemp = istemp;
	pending->atCommit = false;	/* delete if abort */
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->next = pendingDeletes;
	pendingDeletes = pending;
}

/*
 * RelationDropStorage
 *		Schedule unlinking of physical storage at transaction commit.
 */
void
RelationDropStorage(Relation rel)
{
	PendingRelDelete *pending;

	/* Add the relation to the list of stuff to delete at commit */
	pending = (PendingRelDelete *)
		MemoryContextAlloc(TopMemoryContext, sizeof(PendingRelDelete));
	pending->relnode = rel->rd_node;
	pending->isTemp = rel->rd_istemp;
	pending->atCommit = true;	/* delete if commit */
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->next = pendingDeletes;
	pendingDeletes = pending;

	/*
	 * NOTE: if the relation was created in this transaction, it will now be
	 * present in the pending-delete list twice, once with atCommit true and
	 * once with atCommit false.  Hence, it will be physically deleted at end
	 * of xact in either case (and the other entry will be ignored by
	 * smgrDoPendingDeletes, so no error will occur).  We could instead remove
	 * the existing list entry and delete the physical file immediately, but
	 * for now I'll keep the logic simple.
	 */

	RelationCloseSmgr(rel);
}

/*
 * RelationTruncate
 *		Physically truncate a relation to the specified number of blocks.
 *
 * This includes getting rid of any buffers for the blocks that are to be
 * dropped.
 */
void
RelationTruncate(Relation rel, BlockNumber nblocks)
{
	bool		fsm;
	bool		vm;

	/* Open it at the smgr level if not already done */
	RelationOpenSmgr(rel);

	/* Make sure rd_targblock isn't pointing somewhere past end */
	rel->rd_targblock = InvalidBlockNumber;

	/* Truncate the FSM first if it exists */
	fsm = smgrexists(rel->rd_smgr, FSM_FORKNUM);
	if (fsm)
		FreeSpaceMapTruncateRel(rel, nblocks);

	/* Truncate the visibility map too if it exists. */
	vm = smgrexists(rel->rd_smgr, VISIBILITYMAP_FORKNUM);
	if (vm)
		visibilitymap_truncate(rel, nblocks);

	/*
	 * We WAL-log the truncation before actually truncating, which means
	 * trouble if the truncation fails. If we then crash, the WAL replay
	 * likely isn't going to succeed in the truncation either, and cause a
	 * PANIC. It's tempting to put a critical section here, but that cure
	 * would be worse than the disease. It would turn a usually harmless
	 * failure to truncate, that could spell trouble at WAL replay, into a
	 * certain PANIC.
	 */
	if (!rel->rd_istemp)
	{
		/*
		 * Make an XLOG entry showing the file truncation.
		 */
		XLogRecPtr	lsn;
		XLogRecData rdata;
		xl_smgr_truncate xlrec;

		xlrec.blkno = nblocks;
		xlrec.rnode = rel->rd_node;

		rdata.data = (char *) &xlrec;
		rdata.len = sizeof(xlrec);
		rdata.buffer = InvalidBuffer;
		rdata.next = NULL;

		lsn = XLogInsert(RM_SMGR_ID, XLOG_SMGR_TRUNCATE, &rdata);

		/*
		 * Flush, because otherwise the truncation of the main relation might
		 * hit the disk before the WAL record, and the truncation of the FSM
		 * or visibility map. If we crashed during that window, we'd be left
		 * with a truncated heap, but the FSM or visibility map would still
		 * contain entries for the non-existent heap pages.
		 */
		if (fsm || vm)
			XLogFlush(lsn);
	}

	/* Do the real work */
	smgrtruncate(rel->rd_smgr, MAIN_FORKNUM, nblocks, rel->rd_istemp);
}

/*
 *	smgrDoPendingDeletes() -- Take care of relation deletes at end of xact.
 *
 * This also runs when aborting a subxact; we want to clean up a failed
 * subxact immediately.
 */
void
smgrDoPendingDeletes(bool isCommit)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingRelDelete *pending;
	PendingRelDelete *prev;
	PendingRelDelete *next;

	prev = NULL;
	for (pending = pendingDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		if (pending->nestLevel < nestLevel)
		{
			/* outer-level entries should not be processed yet */
			prev = pending;
		}
		else
		{
			/* unlink list entry first, so we don't retry on failure */
			if (prev)
				prev->next = next;
			else
				pendingDeletes = next;
			/* do deletion if called for */
			if (pending->atCommit == isCommit)
			{
				int			i;

				/* schedule unlinking old files */
				SMgrRelation srel;

				srel = smgropen(pending->relnode);
				for (i = 0; i <= MAX_FORKNUM; i++)
				{
					smgrdounlink(srel,
								 i,
								 pending->isTemp,
								 false);
				}
				smgrclose(srel);
			}
			/* must explicitly free the list entry */
			pfree(pending);
			/* prev does not change */
		}
	}
}

/*
 * smgrGetPendingDeletes() -- Get a list of relations to be deleted.
 *
 * The return value is the number of relations scheduled for termination.
 * *ptr is set to point to a freshly-palloc'd array of RelFileNodes.
 * If there are no relations to be deleted, *ptr is set to NULL.
 *
 * If haveNonTemp isn't NULL, the bool it points to gets set to true if
 * there is any non-temp table pending to be deleted; false if not.
 *
 * Note that the list does not include anything scheduled for termination
 * by upper-level transactions.
 */
int
smgrGetPendingDeletes(bool forCommit, RelFileNode **ptr, bool *haveNonTemp)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	int			nrels;
	RelFileNode *rptr;
	PendingRelDelete *pending;

	nrels = 0;
	if (haveNonTemp)
		*haveNonTemp = false;
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel && pending->atCommit == forCommit)
			nrels++;
	}
	if (nrels == 0)
	{
		*ptr = NULL;
		return 0;
	}
	rptr = (RelFileNode *) palloc(nrels * sizeof(RelFileNode));
	*ptr = rptr;
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel && pending->atCommit == forCommit)
		{
			*rptr = pending->relnode;
			rptr++;
		}
		if (haveNonTemp && !pending->isTemp)
			*haveNonTemp = true;
	}
	return nrels;
}

/*
 *	PostPrepare_smgr -- Clean up after a successful PREPARE
 *
 * What we have to do here is throw away the in-memory state about pending
 * relation deletes.  It's all been recorded in the 2PC state file and
 * it's no longer smgr's job to worry about it.
 */
void
PostPrepare_smgr(void)
{
	PendingRelDelete *pending;
	PendingRelDelete *next;

	for (pending = pendingDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		pendingDeletes = next;
		/* must explicitly free the list entry */
		pfree(pending);
	}
}


/*
 * AtSubCommit_smgr() --- Take care of subtransaction commit.
 *
 * Reassign all items in the pending-deletes list to the parent transaction.
 */
void
AtSubCommit_smgr(void)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingRelDelete *pending;

	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel)
			pending->nestLevel = nestLevel - 1;
	}
}

/*
 * AtSubAbort_smgr() --- Take care of subtransaction abort.
 *
 * Delete created relations and forget about deleted relations.
 * We can execute these operations immediately because we know this
 * subtransaction will not commit.
 */
void
AtSubAbort_smgr(void)
{
	smgrDoPendingDeletes(false);
}

void
smgr_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	/* Backup blocks are not used in smgr records */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	if (info == XLOG_SMGR_CREATE)
	{
		xl_smgr_create *xlrec = (xl_smgr_create *) XLogRecGetData(record);
		SMgrRelation reln;

		reln = smgropen(xlrec->rnode);
		smgrcreate(reln, MAIN_FORKNUM, true);
	}
	else if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec = (xl_smgr_truncate *) XLogRecGetData(record);
		SMgrRelation reln;

		reln = smgropen(xlrec->rnode);

		/*
		 * Forcibly create relation if it doesn't exist (which suggests that
		 * it was dropped somewhere later in the WAL sequence).  As in
		 * XLogOpenRelation, we prefer to recreate the rel and replay the log
		 * as best we can until the drop is seen.
		 */
		smgrcreate(reln, MAIN_FORKNUM, true);

		/*
		 * Before we perform the truncation, update minimum recovery point
		 * to cover this WAL record. Once the relation is truncated, there's
		 * no going back. The buffer manager enforces the WAL-first rule
		 * for normal updates to relation files, so that the minimum recovery
		 * point is always updated before the corresponding change in the
		 * data file is flushed to disk. We have to do the same manually
		 * here.
		 *
		 * Doing this before the truncation means that if the truncation fails
		 * for some reason, you cannot start up the system even after restart,
		 * until you fix the underlying situation so that the truncation will
		 * succeed. Alternatively, we could update the minimum recovery point
		 * after truncation, but that would leave a small window where the
		 * WAL-first rule could be violated.
		 */
		XLogFlush(lsn);

		smgrtruncate(reln, MAIN_FORKNUM, xlrec->blkno, false);

		/* Also tell xlogutils.c about it */
		XLogTruncateRelation(xlrec->rnode, MAIN_FORKNUM, xlrec->blkno);

		/* Truncate FSM too */
		if (smgrexists(reln, FSM_FORKNUM))
		{
			Relation	rel = CreateFakeRelcacheEntry(xlrec->rnode);

			FreeSpaceMapTruncateRel(rel, xlrec->blkno);
			FreeFakeRelcacheEntry(rel);
		}

	}
	else
		elog(PANIC, "smgr_redo: unknown op code %u", info);
}

void
smgr_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_SMGR_CREATE)
	{
		xl_smgr_create *xlrec = (xl_smgr_create *) rec;
		char	   *path = relpath(xlrec->rnode, MAIN_FORKNUM);

		appendStringInfo(buf, "file create: %s", path);
		pfree(path);
	}
	else if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec = (xl_smgr_truncate *) rec;
		char	   *path = relpath(xlrec->rnode, MAIN_FORKNUM);

		appendStringInfo(buf, "file truncate: %s to %u blocks", path,
						 xlrec->blkno);
		pfree(path);
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}
