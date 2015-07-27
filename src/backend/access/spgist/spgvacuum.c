/*-------------------------------------------------------------------------
 *
 * spgvacuum.c
 *	  vacuum for SP-GiST
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgvacuum.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/spgist_private.h"
#include "access/transam.h"
#include "access/xloginsert.h"
#include "catalog/storage_xlog.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"


/* Entry in pending-list of TIDs we need to revisit */
typedef struct spgVacPendingItem
{
	ItemPointerData tid;		/* redirection target to visit */
	bool		done;			/* have we dealt with this? */
	struct spgVacPendingItem *next;		/* list link */
} spgVacPendingItem;

/* Local state for vacuum operations */
typedef struct spgBulkDeleteState
{
	/* Parameters passed in to spgvacuumscan */
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;

	/* Additional working state */
	SpGistState spgstate;		/* for SPGiST operations that need one */
	spgVacPendingItem *pendingList;		/* TIDs we need to (re)visit */
	TransactionId myXmin;		/* for detecting newly-added redirects */
	BlockNumber lastFilledBlock;	/* last non-deletable block */
} spgBulkDeleteState;


/*
 * Add TID to pendingList, but only if not already present.
 *
 * Note that new items are always appended at the end of the list; this
 * ensures that scans of the list don't miss items added during the scan.
 */
static void
spgAddPendingTID(spgBulkDeleteState *bds, ItemPointer tid)
{
	spgVacPendingItem *pitem;
	spgVacPendingItem **listLink;

	/* search the list for pre-existing entry */
	listLink = &bds->pendingList;
	while (*listLink != NULL)
	{
		pitem = *listLink;
		if (ItemPointerEquals(tid, &pitem->tid))
			return;				/* already in list, do nothing */
		listLink = &pitem->next;
	}
	/* not there, so append new entry */
	pitem = (spgVacPendingItem *) palloc(sizeof(spgVacPendingItem));
	pitem->tid = *tid;
	pitem->done = false;
	pitem->next = NULL;
	*listLink = pitem;
}

/*
 * Clear pendingList
 */
static void
spgClearPendingList(spgBulkDeleteState *bds)
{
	spgVacPendingItem *pitem;
	spgVacPendingItem *nitem;

	for (pitem = bds->pendingList; pitem != NULL; pitem = nitem)
	{
		nitem = pitem->next;
		/* All items in list should have been dealt with */
		Assert(pitem->done);
		pfree(pitem);
	}
	bds->pendingList = NULL;
}

/*
 * Vacuum a regular (non-root) leaf page
 *
 * We must delete tuples that are targeted for deletion by the VACUUM,
 * but not move any tuples that are referenced by outside links; we assume
 * those are the ones that are heads of chains.
 *
 * If we find a REDIRECT that was made by a concurrently-running transaction,
 * we must add its target TID to pendingList.  (We don't try to visit the
 * target immediately, first because we don't want VACUUM locking more than
 * one buffer at a time, and second because the duplicate-filtering logic
 * in spgAddPendingTID is useful to ensure we can't get caught in an infinite
 * loop in the face of continuous concurrent insertions.)
 *
 * If forPending is true, we are examining the page as a consequence of
 * chasing a redirect link, not as part of the normal sequential scan.
 * We still vacuum the page normally, but we don't increment the stats
 * about live tuples; else we'd double-count those tuples, since the page
 * has been or will be visited in the sequential scan as well.
 */
static void
vacuumLeafPage(spgBulkDeleteState *bds, Relation index, Buffer buffer,
			   bool forPending)
{
	Page		page = BufferGetPage(buffer);
	spgxlogVacuumLeaf xlrec;
	OffsetNumber toDead[MaxIndexTuplesPerPage];
	OffsetNumber toPlaceholder[MaxIndexTuplesPerPage];
	OffsetNumber moveSrc[MaxIndexTuplesPerPage];
	OffsetNumber moveDest[MaxIndexTuplesPerPage];
	OffsetNumber chainSrc[MaxIndexTuplesPerPage];
	OffsetNumber chainDest[MaxIndexTuplesPerPage];
	OffsetNumber predecessor[MaxIndexTuplesPerPage + 1];
	bool		deletable[MaxIndexTuplesPerPage + 1];
	int			nDeletable;
	OffsetNumber i,
				max = PageGetMaxOffsetNumber(page);

	memset(predecessor, 0, sizeof(predecessor));
	memset(deletable, 0, sizeof(deletable));
	nDeletable = 0;

	/* Scan page, identify tuples to delete, accumulate stats */
	for (i = FirstOffsetNumber; i <= max; i++)
	{
		SpGistLeafTuple lt;

		lt = (SpGistLeafTuple) PageGetItem(page,
										   PageGetItemId(page, i));
		if (lt->tupstate == SPGIST_LIVE)
		{
			Assert(ItemPointerIsValid(&lt->heapPtr));

			if (bds->callback(&lt->heapPtr, bds->callback_state))
			{
				bds->stats->tuples_removed += 1;
				deletable[i] = true;
				nDeletable++;
			}
			else
			{
				if (!forPending)
					bds->stats->num_index_tuples += 1;
			}

			/* Form predecessor map, too */
			if (lt->nextOffset != InvalidOffsetNumber)
			{
				/* paranoia about corrupted chain links */
				if (lt->nextOffset < FirstOffsetNumber ||
					lt->nextOffset > max ||
					predecessor[lt->nextOffset] != InvalidOffsetNumber)
					elog(ERROR, "inconsistent tuple chain links in page %u of index \"%s\"",
						 BufferGetBlockNumber(buffer),
						 RelationGetRelationName(index));
				predecessor[lt->nextOffset] = i;
			}
		}
		else if (lt->tupstate == SPGIST_REDIRECT)
		{
			SpGistDeadTuple dt = (SpGistDeadTuple) lt;

			Assert(dt->nextOffset == InvalidOffsetNumber);
			Assert(ItemPointerIsValid(&dt->pointer));

			/*
			 * Add target TID to pending list if the redirection could have
			 * happened since VACUUM started.
			 *
			 * Note: we could make a tighter test by seeing if the xid is
			 * "running" according to the active snapshot; but tqual.c doesn't
			 * currently export a suitable API, and it's not entirely clear
			 * that a tighter test is worth the cycles anyway.
			 */
			if (TransactionIdFollowsOrEquals(dt->xid, bds->myXmin))
				spgAddPendingTID(bds, &dt->pointer);
		}
		else
		{
			Assert(lt->nextOffset == InvalidOffsetNumber);
		}
	}

	if (nDeletable == 0)
		return;					/* nothing more to do */

	/*----------
	 * Figure out exactly what we have to do.  We do this separately from
	 * actually modifying the page, mainly so that we have a representation
	 * that can be dumped into WAL and then the replay code can do exactly
	 * the same thing.  The output of this step consists of six arrays
	 * describing four kinds of operations, to be performed in this order:
	 *
	 * toDead[]: tuple numbers to be replaced with DEAD tuples
	 * toPlaceholder[]: tuple numbers to be replaced with PLACEHOLDER tuples
	 * moveSrc[]: tuple numbers that need to be relocated to another offset
	 * (replacing the tuple there) and then replaced with PLACEHOLDER tuples
	 * moveDest[]: new locations for moveSrc tuples
	 * chainSrc[]: tuple numbers whose chain links (nextOffset) need updates
	 * chainDest[]: new values of nextOffset for chainSrc members
	 *
	 * It's easiest to figure out what we have to do by processing tuple
	 * chains, so we iterate over all the tuples (not just the deletable
	 * ones!) to identify chain heads, then chase down each chain and make
	 * work item entries for deletable tuples within the chain.
	 *----------
	 */
	xlrec.nDead = xlrec.nPlaceholder = xlrec.nMove = xlrec.nChain = 0;

	for (i = FirstOffsetNumber; i <= max; i++)
	{
		SpGistLeafTuple head;
		bool		interveningDeletable;
		OffsetNumber prevLive;
		OffsetNumber j;

		head = (SpGistLeafTuple) PageGetItem(page,
											 PageGetItemId(page, i));
		if (head->tupstate != SPGIST_LIVE)
			continue;			/* can't be a chain member */
		if (predecessor[i] != 0)
			continue;			/* not a chain head */

		/* initialize ... */
		interveningDeletable = false;
		prevLive = deletable[i] ? InvalidOffsetNumber : i;

		/* scan down the chain ... */
		j = head->nextOffset;
		while (j != InvalidOffsetNumber)
		{
			SpGistLeafTuple lt;

			lt = (SpGistLeafTuple) PageGetItem(page,
											   PageGetItemId(page, j));
			if (lt->tupstate != SPGIST_LIVE)
			{
				/* all tuples in chain should be live */
				elog(ERROR, "unexpected SPGiST tuple state: %d",
					 lt->tupstate);
			}

			if (deletable[j])
			{
				/* This tuple should be replaced by a placeholder */
				toPlaceholder[xlrec.nPlaceholder] = j;
				xlrec.nPlaceholder++;
				/* previous live tuple's chain link will need an update */
				interveningDeletable = true;
			}
			else if (prevLive == InvalidOffsetNumber)
			{
				/*
				 * This is the first live tuple in the chain.  It has to move
				 * to the head position.
				 */
				moveSrc[xlrec.nMove] = j;
				moveDest[xlrec.nMove] = i;
				xlrec.nMove++;
				/* Chain updates will be applied after the move */
				prevLive = i;
				interveningDeletable = false;
			}
			else
			{
				/*
				 * Second or later live tuple.  Arrange to re-chain it to the
				 * previous live one, if there was a gap.
				 */
				if (interveningDeletable)
				{
					chainSrc[xlrec.nChain] = prevLive;
					chainDest[xlrec.nChain] = j;
					xlrec.nChain++;
				}
				prevLive = j;
				interveningDeletable = false;
			}

			j = lt->nextOffset;
		}

		if (prevLive == InvalidOffsetNumber)
		{
			/* The chain is entirely removable, so we need a DEAD tuple */
			toDead[xlrec.nDead] = i;
			xlrec.nDead++;
		}
		else if (interveningDeletable)
		{
			/* One or more deletions at end of chain, so close it off */
			chainSrc[xlrec.nChain] = prevLive;
			chainDest[xlrec.nChain] = InvalidOffsetNumber;
			xlrec.nChain++;
		}
	}

	/* sanity check ... */
	if (nDeletable != xlrec.nDead + xlrec.nPlaceholder + xlrec.nMove)
		elog(ERROR, "inconsistent counts of deletable tuples");

	/* Do the updates */
	START_CRIT_SECTION();

	spgPageIndexMultiDelete(&bds->spgstate, page,
							toDead, xlrec.nDead,
							SPGIST_DEAD, SPGIST_DEAD,
							InvalidBlockNumber, InvalidOffsetNumber);

	spgPageIndexMultiDelete(&bds->spgstate, page,
							toPlaceholder, xlrec.nPlaceholder,
							SPGIST_PLACEHOLDER, SPGIST_PLACEHOLDER,
							InvalidBlockNumber, InvalidOffsetNumber);

	/*
	 * We implement the move step by swapping the item pointers of the source
	 * and target tuples, then replacing the newly-source tuples with
	 * placeholders.  This is perhaps unduly friendly with the page data
	 * representation, but it's fast and doesn't risk page overflow when a
	 * tuple to be relocated is large.
	 */
	for (i = 0; i < xlrec.nMove; i++)
	{
		ItemId		idSrc = PageGetItemId(page, moveSrc[i]);
		ItemId		idDest = PageGetItemId(page, moveDest[i]);
		ItemIdData	tmp;

		tmp = *idSrc;
		*idSrc = *idDest;
		*idDest = tmp;
	}

	spgPageIndexMultiDelete(&bds->spgstate, page,
							moveSrc, xlrec.nMove,
							SPGIST_PLACEHOLDER, SPGIST_PLACEHOLDER,
							InvalidBlockNumber, InvalidOffsetNumber);

	for (i = 0; i < xlrec.nChain; i++)
	{
		SpGistLeafTuple lt;

		lt = (SpGistLeafTuple) PageGetItem(page,
										   PageGetItemId(page, chainSrc[i]));
		Assert(lt->tupstate == SPGIST_LIVE);
		lt->nextOffset = chainDest[i];
	}

	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(index))
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();

		STORE_STATE(&bds->spgstate, xlrec.stateSrc);

		XLogRegisterData((char *) &xlrec, SizeOfSpgxlogVacuumLeaf);
		/* sizeof(xlrec) should be a multiple of sizeof(OffsetNumber) */
		XLogRegisterData((char *) toDead, sizeof(OffsetNumber) * xlrec.nDead);
		XLogRegisterData((char *) toPlaceholder, sizeof(OffsetNumber) * xlrec.nPlaceholder);
		XLogRegisterData((char *) moveSrc, sizeof(OffsetNumber) * xlrec.nMove);
		XLogRegisterData((char *) moveDest, sizeof(OffsetNumber) * xlrec.nMove);
		XLogRegisterData((char *) chainSrc, sizeof(OffsetNumber) * xlrec.nChain);
		XLogRegisterData((char *) chainDest, sizeof(OffsetNumber) * xlrec.nChain);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_VACUUM_LEAF);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();
}

/*
 * Vacuum a root page when it is also a leaf
 *
 * On the root, we just delete any dead leaf tuples; no fancy business
 */
static void
vacuumLeafRoot(spgBulkDeleteState *bds, Relation index, Buffer buffer)
{
	Page		page = BufferGetPage(buffer);
	spgxlogVacuumRoot xlrec;
	OffsetNumber toDelete[MaxIndexTuplesPerPage];
	OffsetNumber i,
				max = PageGetMaxOffsetNumber(page);

	xlrec.nDelete = 0;

	/* Scan page, identify tuples to delete, accumulate stats */
	for (i = FirstOffsetNumber; i <= max; i++)
	{
		SpGistLeafTuple lt;

		lt = (SpGistLeafTuple) PageGetItem(page,
										   PageGetItemId(page, i));
		if (lt->tupstate == SPGIST_LIVE)
		{
			Assert(ItemPointerIsValid(&lt->heapPtr));

			if (bds->callback(&lt->heapPtr, bds->callback_state))
			{
				bds->stats->tuples_removed += 1;
				toDelete[xlrec.nDelete] = i;
				xlrec.nDelete++;
			}
			else
			{
				bds->stats->num_index_tuples += 1;
			}
		}
		else
		{
			/* all tuples on root should be live */
			elog(ERROR, "unexpected SPGiST tuple state: %d",
				 lt->tupstate);
		}
	}

	if (xlrec.nDelete == 0)
		return;					/* nothing more to do */

	/* Do the update */
	START_CRIT_SECTION();

	/* The tuple numbers are in order, so we can use PageIndexMultiDelete */
	PageIndexMultiDelete(page, toDelete, xlrec.nDelete);

	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(index))
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();

		/* Prepare WAL record */
		STORE_STATE(&bds->spgstate, xlrec.stateSrc);

		XLogRegisterData((char *) &xlrec, SizeOfSpgxlogVacuumRoot);
		/* sizeof(xlrec) should be a multiple of sizeof(OffsetNumber) */
		XLogRegisterData((char *) toDelete,
						 sizeof(OffsetNumber) * xlrec.nDelete);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_VACUUM_ROOT);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();
}

/*
 * Clean up redirect and placeholder tuples on the given page
 *
 * Redirect tuples can be marked placeholder once they're old enough.
 * Placeholder tuples can be removed if it won't change the offsets of
 * non-placeholder ones.
 *
 * Unlike the routines above, this works on both leaf and inner pages.
 */
static void
vacuumRedirectAndPlaceholder(Relation index, Buffer buffer)
{
	Page		page = BufferGetPage(buffer);
	SpGistPageOpaque opaque = SpGistPageGetOpaque(page);
	OffsetNumber i,
				max = PageGetMaxOffsetNumber(page),
				firstPlaceholder = InvalidOffsetNumber;
	bool		hasNonPlaceholder = false;
	bool		hasUpdate = false;
	OffsetNumber itemToPlaceholder[MaxIndexTuplesPerPage];
	OffsetNumber itemnos[MaxIndexTuplesPerPage];
	spgxlogVacuumRedirect xlrec;

	xlrec.nToPlaceholder = 0;
	xlrec.newestRedirectXid = InvalidTransactionId;

	START_CRIT_SECTION();

	/*
	 * Scan backwards to convert old redirection tuples to placeholder tuples,
	 * and identify location of last non-placeholder tuple while at it.
	 */
	for (i = max;
		 i >= FirstOffsetNumber &&
		 (opaque->nRedirection > 0 || !hasNonPlaceholder);
		 i--)
	{
		SpGistDeadTuple dt;

		dt = (SpGistDeadTuple) PageGetItem(page, PageGetItemId(page, i));

		if (dt->tupstate == SPGIST_REDIRECT &&
			TransactionIdPrecedes(dt->xid, RecentGlobalXmin))
		{
			dt->tupstate = SPGIST_PLACEHOLDER;
			Assert(opaque->nRedirection > 0);
			opaque->nRedirection--;
			opaque->nPlaceholder++;

			/* remember newest XID among the removed redirects */
			if (!TransactionIdIsValid(xlrec.newestRedirectXid) ||
				TransactionIdPrecedes(xlrec.newestRedirectXid, dt->xid))
				xlrec.newestRedirectXid = dt->xid;

			ItemPointerSetInvalid(&dt->pointer);

			itemToPlaceholder[xlrec.nToPlaceholder] = i;
			xlrec.nToPlaceholder++;

			hasUpdate = true;
		}

		if (dt->tupstate == SPGIST_PLACEHOLDER)
		{
			if (!hasNonPlaceholder)
				firstPlaceholder = i;
		}
		else
		{
			hasNonPlaceholder = true;
		}
	}

	/*
	 * Any placeholder tuples at the end of page can safely be removed.  We
	 * can't remove ones before the last non-placeholder, though, because we
	 * can't alter the offset numbers of non-placeholder tuples.
	 */
	if (firstPlaceholder != InvalidOffsetNumber)
	{
		/*
		 * We do not store this array to rdata because it's easy to recreate.
		 */
		for (i = firstPlaceholder; i <= max; i++)
			itemnos[i - firstPlaceholder] = i;

		i = max - firstPlaceholder + 1;
		Assert(opaque->nPlaceholder >= i);
		opaque->nPlaceholder -= i;

		/* The array is surely sorted, so can use PageIndexMultiDelete */
		PageIndexMultiDelete(page, itemnos, i);

		hasUpdate = true;
	}

	xlrec.firstPlaceholder = firstPlaceholder;

	if (hasUpdate)
		MarkBufferDirty(buffer);

	if (hasUpdate && RelationNeedsWAL(index))
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();

		XLogRegisterData((char *) &xlrec, SizeOfSpgxlogVacuumRedirect);
		XLogRegisterData((char *) itemToPlaceholder,
						 sizeof(OffsetNumber) * xlrec.nToPlaceholder);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_VACUUM_REDIRECT);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();
}

/*
 * Process one page during a bulkdelete scan
 */
static void
spgvacuumpage(spgBulkDeleteState *bds, BlockNumber blkno)
{
	Relation	index = bds->info->index;
	Buffer		buffer;
	Page		page;

	/* call vacuum_delay_point while not holding any buffer lock */
	vacuum_delay_point();

	buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
								RBM_NORMAL, bds->info->strategy);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = (Page) BufferGetPage(buffer);

	if (PageIsNew(page))
	{
		/*
		 * We found an all-zero page, which could happen if the database
		 * crashed just after extending the file.  Recycle it.
		 */
	}
	else if (PageIsEmpty(page))
	{
		/* nothing to do */
	}
	else if (SpGistPageIsLeaf(page))
	{
		if (SpGistBlockIsRoot(blkno))
		{
			vacuumLeafRoot(bds, index, buffer);
			/* no need for vacuumRedirectAndPlaceholder */
		}
		else
		{
			vacuumLeafPage(bds, index, buffer, false);
			vacuumRedirectAndPlaceholder(index, buffer);
		}
	}
	else
	{
		/* inner page */
		vacuumRedirectAndPlaceholder(index, buffer);
	}

	/*
	 * The root pages must never be deleted, nor marked as available in FSM,
	 * because we don't want them ever returned by a search for a place to put
	 * a new tuple.  Otherwise, check for empty page, and make sure the FSM
	 * knows about it.
	 */
	if (!SpGistBlockIsRoot(blkno))
	{
		if (PageIsNew(page) || PageIsEmpty(page))
		{
			RecordFreeIndexPage(index, blkno);
			bds->stats->pages_deleted++;
		}
		else
		{
			SpGistSetLastUsedPage(index, buffer);
			bds->lastFilledBlock = blkno;
		}
	}

	UnlockReleaseBuffer(buffer);
}

/*
 * Process the pending-TID list between pages of the main scan
 */
static void
spgprocesspending(spgBulkDeleteState *bds)
{
	Relation	index = bds->info->index;
	spgVacPendingItem *pitem;
	spgVacPendingItem *nitem;
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;

	for (pitem = bds->pendingList; pitem != NULL; pitem = pitem->next)
	{
		if (pitem->done)
			continue;			/* ignore already-done items */

		/* call vacuum_delay_point while not holding any buffer lock */
		vacuum_delay_point();

		/* examine the referenced page */
		blkno = ItemPointerGetBlockNumber(&pitem->tid);
		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, bds->info->strategy);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = (Page) BufferGetPage(buffer);

		if (PageIsNew(page) || SpGistPageIsDeleted(page))
		{
			/* Probably shouldn't happen, but ignore it */
		}
		else if (SpGistPageIsLeaf(page))
		{
			if (SpGistBlockIsRoot(blkno))
			{
				/* this should definitely not happen */
				elog(ERROR, "redirection leads to root page of index \"%s\"",
					 RelationGetRelationName(index));
			}

			/* deal with any deletable tuples */
			vacuumLeafPage(bds, index, buffer, true);
			/* might as well do this while we are here */
			vacuumRedirectAndPlaceholder(index, buffer);

			SpGistSetLastUsedPage(index, buffer);

			/*
			 * We can mark as done not only this item, but any later ones
			 * pointing at the same page, since we vacuumed the whole page.
			 */
			pitem->done = true;
			for (nitem = pitem->next; nitem != NULL; nitem = nitem->next)
			{
				if (ItemPointerGetBlockNumber(&nitem->tid) == blkno)
					nitem->done = true;
			}
		}
		else
		{
			/*
			 * On an inner page, visit the referenced inner tuple and add all
			 * its downlinks to the pending list.  We might have pending items
			 * for more than one inner tuple on the same page (in fact this is
			 * pretty likely given the way space allocation works), so get
			 * them all while we are here.
			 */
			for (nitem = pitem; nitem != NULL; nitem = nitem->next)
			{
				if (nitem->done)
					continue;
				if (ItemPointerGetBlockNumber(&nitem->tid) == blkno)
				{
					OffsetNumber offset;
					SpGistInnerTuple innerTuple;

					offset = ItemPointerGetOffsetNumber(&nitem->tid);
					innerTuple = (SpGistInnerTuple) PageGetItem(page,
												PageGetItemId(page, offset));
					if (innerTuple->tupstate == SPGIST_LIVE)
					{
						SpGistNodeTuple node;
						int			i;

						SGITITERATE(innerTuple, i, node)
						{
							if (ItemPointerIsValid(&node->t_tid))
								spgAddPendingTID(bds, &node->t_tid);
						}
					}
					else if (innerTuple->tupstate == SPGIST_REDIRECT)
					{
						/* transfer attention to redirect point */
						spgAddPendingTID(bds,
								   &((SpGistDeadTuple) innerTuple)->pointer);
					}
					else
						elog(ERROR, "unexpected SPGiST tuple state: %d",
							 innerTuple->tupstate);

					nitem->done = true;
				}
			}
		}

		UnlockReleaseBuffer(buffer);
	}

	spgClearPendingList(bds);
}

/*
 * Perform a bulkdelete scan
 */
static void
spgvacuumscan(spgBulkDeleteState *bds)
{
	Relation	index = bds->info->index;
	bool		needLock;
	BlockNumber num_pages,
				blkno;

	/* Finish setting up spgBulkDeleteState */
	initSpGistState(&bds->spgstate, index);
	bds->pendingList = NULL;
	bds->myXmin = GetActiveSnapshot()->xmin;
	bds->lastFilledBlock = SPGIST_LAST_FIXED_BLKNO;

	/*
	 * Reset counts that will be incremented during the scan; needed in case
	 * of multiple scans during a single VACUUM command
	 */
	bds->stats->estimated_count = false;
	bds->stats->num_index_tuples = 0;
	bds->stats->pages_deleted = 0;

	/* We can skip locking for new or temp relations */
	needLock = !RELATION_IS_LOCAL(index);

	/*
	 * The outer loop iterates over all index pages except the metapage, in
	 * physical order (we hope the kernel will cooperate in providing
	 * read-ahead for speed).  It is critical that we visit all leaf pages,
	 * including ones added after we start the scan, else we might fail to
	 * delete some deletable tuples.  See more extensive comments about this
	 * in btvacuumscan().
	 */
	blkno = SPGIST_METAPAGE_BLKNO + 1;
	for (;;)
	{
		/* Get the current relation length */
		if (needLock)
			LockRelationForExtension(index, ExclusiveLock);
		num_pages = RelationGetNumberOfBlocks(index);
		if (needLock)
			UnlockRelationForExtension(index, ExclusiveLock);

		/* Quit if we've scanned the whole relation */
		if (blkno >= num_pages)
			break;
		/* Iterate over pages, then loop back to recheck length */
		for (; blkno < num_pages; blkno++)
		{
			spgvacuumpage(bds, blkno);
			/* empty the pending-list after each page */
			if (bds->pendingList != NULL)
				spgprocesspending(bds);
		}
	}

	/* Propagate local lastUsedPage cache to metablock */
	SpGistUpdateMetaPage(index);

	/*
	 * Truncate index if possible
	 *
	 * XXX disabled because it's unsafe due to possible concurrent inserts.
	 * We'd have to rescan the pages to make sure they're still empty, and it
	 * doesn't seem worth it.  Note that btree doesn't do this either.
	 *
	 * Another reason not to truncate is that it could invalidate the cached
	 * pages-with-freespace pointers in the metapage and other backends'
	 * relation caches, that is leave them pointing to nonexistent pages.
	 * Adding RelationGetNumberOfBlocks calls to protect the places that use
	 * those pointers would be unduly expensive.
	 */
#ifdef NOT_USED
	if (num_pages > bds->lastFilledBlock + 1)
	{
		BlockNumber lastBlock = num_pages - 1;

		num_pages = bds->lastFilledBlock + 1;
		RelationTruncate(index, num_pages);
		bds->stats->pages_removed += lastBlock - bds->lastFilledBlock;
		bds->stats->pages_deleted -= lastBlock - bds->lastFilledBlock;
	}
#endif

	/* Report final stats */
	bds->stats->num_pages = num_pages;
	bds->stats->pages_free = bds->stats->pages_deleted;
}

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
Datum
spgbulkdelete(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *) PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(1);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback) PG_GETARG_POINTER(2);
	void	   *callback_state = (void *) PG_GETARG_POINTER(3);
	spgBulkDeleteState bds;

	/* allocate stats if first time through, else re-use existing struct */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	bds.info = info;
	bds.stats = stats;
	bds.callback = callback;
	bds.callback_state = callback_state;

	spgvacuumscan(&bds);

	PG_RETURN_POINTER(stats);
}

/* Dummy callback to delete no tuples during spgvacuumcleanup */
static bool
dummy_callback(ItemPointer itemptr, void *state)
{
	return false;
}

/*
 * Post-VACUUM cleanup.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
Datum
spgvacuumcleanup(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *) PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(1);
	Relation	index = info->index;
	spgBulkDeleteState bds;

	/* No-op in ANALYZE ONLY mode */
	if (info->analyze_only)
		PG_RETURN_POINTER(stats);

	/*
	 * We don't need to scan the index if there was a preceding bulkdelete
	 * pass.  Otherwise, make a pass that won't delete any live tuples, but
	 * might still accomplish useful stuff with redirect/placeholder cleanup,
	 * and in any case will provide stats.
	 */
	if (stats == NULL)
	{
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		bds.info = info;
		bds.stats = stats;
		bds.callback = dummy_callback;
		bds.callback_state = NULL;

		spgvacuumscan(&bds);
	}

	/* Finally, vacuum the FSM */
	IndexFreeSpaceMapVacuum(index);

	/*
	 * It's quite possible for us to be fooled by concurrent tuple moves into
	 * double-counting some index tuples, so disbelieve any total that exceeds
	 * the underlying heap's count ... if we know that accurately.  Otherwise
	 * this might just make matters worse.
	 */
	if (!info->estimated_count)
	{
		if (stats->num_index_tuples > info->num_heap_tuples)
			stats->num_index_tuples = info->num_heap_tuples;
	}

	PG_RETURN_POINTER(stats);
}
