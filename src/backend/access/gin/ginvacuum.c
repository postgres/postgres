/*-------------------------------------------------------------------------
 *
 * ginvacuum.c
 *	  delete & vacuum routines for the postgres GIN
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginvacuum.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/ginxlog.h"
#include "access/xloginsert.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/memutils.h"

struct GinVacuumState
{
	Relation	index;
	IndexBulkDeleteResult *result;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	GinState	ginstate;
	BufferAccessStrategy strategy;
	MemoryContext tmpCxt;
};

/*
 * Vacuums an uncompressed posting list. The size of the must can be specified
 * in number of items (nitems).
 *
 * If none of the items need to be removed, returns NULL. Otherwise returns
 * a new palloc'd array with the remaining items. The number of remaining
 * items is returned in *nremaining.
 */
ItemPointer
ginVacuumItemPointers(GinVacuumState *gvs, ItemPointerData *items,
					  int nitem, int *nremaining)
{
	int			i,
				remaining = 0;
	ItemPointer tmpitems = NULL;

	/*
	 * Iterate over TIDs array
	 */
	for (i = 0; i < nitem; i++)
	{
		if (gvs->callback(items + i, gvs->callback_state))
		{
			gvs->result->tuples_removed += 1;
			if (!tmpitems)
			{
				/*
				 * First TID to be deleted: allocate memory to hold the
				 * remaining items.
				 */
				tmpitems = palloc(sizeof(ItemPointerData) * nitem);
				memcpy(tmpitems, items, sizeof(ItemPointerData) * i);
			}
		}
		else
		{
			gvs->result->num_index_tuples += 1;
			if (tmpitems)
				tmpitems[remaining] = items[i];
			remaining++;
		}
	}

	*nremaining = remaining;
	return tmpitems;
}

/*
 * Create a WAL record for vacuuming entry tree leaf page.
 */
static void
xlogVacuumPage(Relation index, Buffer buffer)
{
	Page		page = BufferGetPage(buffer);
	XLogRecPtr	recptr;

	/* This is only used for entry tree leaf pages. */
	Assert(!GinPageIsData(page));
	Assert(GinPageIsLeaf(page));

	if (!RelationNeedsWAL(index))
		return;

	/*
	 * Always create a full image, we don't track the changes on the page at
	 * any more fine-grained level. This could obviously be improved...
	 */
	XLogBeginInsert();
	XLogRegisterBuffer(0, buffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);

	recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_VACUUM_PAGE);
	PageSetLSN(page, recptr);
}


typedef struct DataPageDeleteStack
{
	struct DataPageDeleteStack *child;
	struct DataPageDeleteStack *parent;

	BlockNumber blkno;			/* current block number */
	Buffer		leftBuffer;		/* pinned and locked rightest non-deleted page
								 * on left */
	bool		isRoot;
} DataPageDeleteStack;


/*
 * Delete a posting tree page.
 */
static void
ginDeletePage(GinVacuumState *gvs, BlockNumber deleteBlkno, BlockNumber leftBlkno,
			  BlockNumber parentBlkno, OffsetNumber myoff, bool isParentRoot)
{
	Buffer		dBuffer;
	Buffer		lBuffer;
	Buffer		pBuffer;
	Page		page,
				parentPage;
	BlockNumber rightlink;

	/*
	 * This function MUST be called only if someone of parent pages hold
	 * exclusive cleanup lock. This guarantees that no insertions currently
	 * happen in this subtree. Caller also acquires Exclusive locks on
	 * deletable, parent and left pages.
	 */
	lBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, leftBlkno,
								 RBM_NORMAL, gvs->strategy);
	dBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, deleteBlkno,
								 RBM_NORMAL, gvs->strategy);
	pBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, parentBlkno,
								 RBM_NORMAL, gvs->strategy);

	page = BufferGetPage(dBuffer);
	rightlink = GinPageGetOpaque(page)->rightlink;

	/*
	 * Any insert which would have gone on the leaf block will now go to its
	 * right sibling.
	 */
	PredicateLockPageCombine(gvs->index, deleteBlkno, rightlink);

	START_CRIT_SECTION();

	/* Unlink the page by changing left sibling's rightlink */
	page = BufferGetPage(lBuffer);
	GinPageGetOpaque(page)->rightlink = rightlink;

	/* Delete downlink from parent */
	parentPage = BufferGetPage(pBuffer);
#ifdef USE_ASSERT_CHECKING
	do
	{
		PostingItem *tod = GinDataPageGetPostingItem(parentPage, myoff);

		Assert(PostingItemGetBlockNumber(tod) == deleteBlkno);
	} while (0);
#endif
	GinPageDeletePostingItem(parentPage, myoff);

	page = BufferGetPage(dBuffer);

	/*
	 * we shouldn't change rightlink field to save workability of running
	 * search scan
	 */

	/*
	 * Mark page as deleted, and remember last xid which could know its
	 * address.
	 */
	GinPageSetDeleted(page);
	GinPageSetDeleteXid(page, ReadNextTransactionId());

	MarkBufferDirty(pBuffer);
	MarkBufferDirty(lBuffer);
	MarkBufferDirty(dBuffer);

	if (RelationNeedsWAL(gvs->index))
	{
		XLogRecPtr	recptr;
		ginxlogDeletePage data;

		/*
		 * We can't pass REGBUF_STANDARD for the deleted page, because we
		 * didn't set pd_lower on pre-9.4 versions. The page might've been
		 * binary-upgraded from an older version, and hence not have pd_lower
		 * set correctly. Ditto for the left page, but removing the item from
		 * the parent updated its pd_lower, so we know that's OK at this
		 * point.
		 */
		XLogBeginInsert();
		XLogRegisterBuffer(0, dBuffer, 0);
		XLogRegisterBuffer(1, pBuffer, REGBUF_STANDARD);
		XLogRegisterBuffer(2, lBuffer, 0);

		data.parentOffset = myoff;
		data.rightLink = GinPageGetOpaque(page)->rightlink;
		data.deleteXid = GinPageGetDeleteXid(page);

		XLogRegisterData((char *) &data, sizeof(ginxlogDeletePage));

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_DELETE_PAGE);
		PageSetLSN(page, recptr);
		PageSetLSN(parentPage, recptr);
		PageSetLSN(BufferGetPage(lBuffer), recptr);
	}

	ReleaseBuffer(pBuffer);
	ReleaseBuffer(lBuffer);
	ReleaseBuffer(dBuffer);

	END_CRIT_SECTION();

	gvs->result->pages_newly_deleted++;
	gvs->result->pages_deleted++;
}


/*
 * Scans posting tree and deletes empty pages.  Caller must lock root page for
 * cleanup.  During scan path from root to current page is kept exclusively
 * locked.  Also keep left page exclusively locked, because ginDeletePage()
 * needs it.  If we try to relock left page later, it could deadlock with
 * ginStepRight().
 */
static bool
ginScanToDelete(GinVacuumState *gvs, BlockNumber blkno, bool isRoot,
				DataPageDeleteStack *parent, OffsetNumber myoff)
{
	DataPageDeleteStack *me;
	Buffer		buffer;
	Page		page;
	bool		meDelete = false;
	bool		isempty;

	if (isRoot)
	{
		me = parent;
	}
	else
	{
		if (!parent->child)
		{
			me = (DataPageDeleteStack *) palloc0(sizeof(DataPageDeleteStack));
			me->parent = parent;
			parent->child = me;
			me->leftBuffer = InvalidBuffer;
		}
		else
			me = parent->child;
	}

	buffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, blkno,
								RBM_NORMAL, gvs->strategy);

	if (!isRoot)
		LockBuffer(buffer, GIN_EXCLUSIVE);

	page = BufferGetPage(buffer);

	Assert(GinPageIsData(page));

	if (!GinPageIsLeaf(page))
	{
		OffsetNumber i;

		me->blkno = blkno;
		for (i = FirstOffsetNumber; i <= GinPageGetOpaque(page)->maxoff; i++)
		{
			PostingItem *pitem = GinDataPageGetPostingItem(page, i);

			if (ginScanToDelete(gvs, PostingItemGetBlockNumber(pitem), false, me, i))
				i--;
		}

		if (GinPageRightMost(page) && BufferIsValid(me->child->leftBuffer))
		{
			UnlockReleaseBuffer(me->child->leftBuffer);
			me->child->leftBuffer = InvalidBuffer;
		}
	}

	if (GinPageIsLeaf(page))
		isempty = GinDataLeafPageIsEmpty(page);
	else
		isempty = GinPageGetOpaque(page)->maxoff < FirstOffsetNumber;

	if (isempty)
	{
		/* we never delete the left- or rightmost branch */
		if (BufferIsValid(me->leftBuffer) && !GinPageRightMost(page))
		{
			Assert(!isRoot);
			ginDeletePage(gvs, blkno, BufferGetBlockNumber(me->leftBuffer),
						  me->parent->blkno, myoff, me->parent->isRoot);
			meDelete = true;
		}
	}

	if (!meDelete)
	{
		if (BufferIsValid(me->leftBuffer))
			UnlockReleaseBuffer(me->leftBuffer);
		me->leftBuffer = buffer;
	}
	else
	{
		if (!isRoot)
			LockBuffer(buffer, GIN_UNLOCK);

		ReleaseBuffer(buffer);
	}

	if (isRoot)
		ReleaseBuffer(buffer);

	return meDelete;
}


/*
 * Scan through posting tree leafs, delete empty tuples.  Returns true if there
 * is at least one empty page.
 */
static bool
ginVacuumPostingTreeLeaves(GinVacuumState *gvs, BlockNumber blkno)
{
	Buffer		buffer;
	Page		page;
	bool		hasVoidPage = false;
	MemoryContext oldCxt;

	/* Find leftmost leaf page of posting tree and lock it in exclusive mode */
	while (true)
	{
		PostingItem *pitem;

		buffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, gvs->strategy);
		LockBuffer(buffer, GIN_SHARE);
		page = BufferGetPage(buffer);

		Assert(GinPageIsData(page));

		if (GinPageIsLeaf(page))
		{
			LockBuffer(buffer, GIN_UNLOCK);
			LockBuffer(buffer, GIN_EXCLUSIVE);
			break;
		}

		Assert(PageGetMaxOffsetNumber(page) >= FirstOffsetNumber);

		pitem = GinDataPageGetPostingItem(page, FirstOffsetNumber);
		blkno = PostingItemGetBlockNumber(pitem);
		Assert(blkno != InvalidBlockNumber);

		UnlockReleaseBuffer(buffer);
	}

	/* Iterate all posting tree leaves using rightlinks and vacuum them */
	while (true)
	{
		oldCxt = MemoryContextSwitchTo(gvs->tmpCxt);
		ginVacuumPostingTreeLeaf(gvs->index, buffer, gvs);
		MemoryContextSwitchTo(oldCxt);
		MemoryContextReset(gvs->tmpCxt);

		if (GinDataLeafPageIsEmpty(page))
			hasVoidPage = true;

		blkno = GinPageGetOpaque(page)->rightlink;

		UnlockReleaseBuffer(buffer);

		if (blkno == InvalidBlockNumber)
			break;

		buffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, gvs->strategy);
		LockBuffer(buffer, GIN_EXCLUSIVE);
		page = BufferGetPage(buffer);
	}

	return hasVoidPage;
}

static void
ginVacuumPostingTree(GinVacuumState *gvs, BlockNumber rootBlkno)
{
	if (ginVacuumPostingTreeLeaves(gvs, rootBlkno))
	{
		/*
		 * There is at least one empty page.  So we have to rescan the tree
		 * deleting empty pages.
		 */
		Buffer		buffer;
		DataPageDeleteStack root,
				   *ptr,
				   *tmp;

		buffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, rootBlkno,
									RBM_NORMAL, gvs->strategy);

		/*
		 * Lock posting tree root for cleanup to ensure there are no
		 * concurrent inserts.
		 */
		LockBufferForCleanup(buffer);

		memset(&root, 0, sizeof(DataPageDeleteStack));
		root.leftBuffer = InvalidBuffer;
		root.isRoot = true;

		ginScanToDelete(gvs, rootBlkno, true, &root, InvalidOffsetNumber);

		ptr = root.child;

		while (ptr)
		{
			tmp = ptr->child;
			pfree(ptr);
			ptr = tmp;
		}

		UnlockReleaseBuffer(buffer);
	}
}

/*
 * returns modified page or NULL if page isn't modified.
 * Function works with original page until first change is occurred,
 * then page is copied into temporary one.
 */
static Page
ginVacuumEntryPage(GinVacuumState *gvs, Buffer buffer, BlockNumber *roots, uint32 *nroot)
{
	Page		origpage = BufferGetPage(buffer),
				tmppage;
	OffsetNumber i,
				maxoff = PageGetMaxOffsetNumber(origpage);

	tmppage = origpage;

	*nroot = 0;

	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(tmppage, PageGetItemId(tmppage, i));

		if (GinIsPostingTree(itup))
		{
			/*
			 * store posting tree's roots for further processing, we can't
			 * vacuum it just now due to risk of deadlocks with scans/inserts
			 */
			roots[*nroot] = GinGetDownlink(itup);
			(*nroot)++;
		}
		else if (GinGetNPosting(itup) > 0)
		{
			int			nitems;
			ItemPointer items_orig;
			bool		free_items_orig;
			ItemPointer items;

			/* Get list of item pointers from the tuple. */
			if (GinItupIsCompressed(itup))
			{
				items_orig = ginPostingListDecode((GinPostingList *) GinGetPosting(itup), &nitems);
				free_items_orig = true;
			}
			else
			{
				items_orig = (ItemPointer) GinGetPosting(itup);
				nitems = GinGetNPosting(itup);
				free_items_orig = false;
			}

			/* Remove any items from the list that need to be vacuumed. */
			items = ginVacuumItemPointers(gvs, items_orig, nitems, &nitems);

			if (free_items_orig)
				pfree(items_orig);

			/* If any item pointers were removed, recreate the tuple. */
			if (items)
			{
				OffsetNumber attnum;
				Datum		key;
				GinNullCategory category;
				GinPostingList *plist;
				int			plistsize;

				if (nitems > 0)
				{
					plist = ginCompressPostingList(items, nitems, GinMaxItemSize, NULL);
					plistsize = SizeOfGinPostingList(plist);
				}
				else
				{
					plist = NULL;
					plistsize = 0;
				}

				/*
				 * if we already created a temporary page, make changes in
				 * place
				 */
				if (tmppage == origpage)
				{
					/*
					 * On first difference, create a temporary copy of the
					 * page and copy the tuple's posting list to it.
					 */
					tmppage = PageGetTempPageCopy(origpage);

					/* set itup pointer to new page */
					itup = (IndexTuple) PageGetItem(tmppage, PageGetItemId(tmppage, i));
				}

				attnum = gintuple_get_attrnum(&gvs->ginstate, itup);
				key = gintuple_get_key(&gvs->ginstate, itup, &category);
				itup = GinFormTuple(&gvs->ginstate, attnum, key, category,
									(char *) plist, plistsize,
									nitems, true);
				if (plist)
					pfree(plist);
				PageIndexTupleDelete(tmppage, i);

				if (PageAddItem(tmppage, (Item) itup, IndexTupleSize(itup), i, false, false) != i)
					elog(ERROR, "failed to add item to index page in \"%s\"",
						 RelationGetRelationName(gvs->index));

				pfree(itup);
				pfree(items);
			}
		}
	}

	return (tmppage == origpage) ? NULL : tmppage;
}

IndexBulkDeleteResult *
ginbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			  IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	BlockNumber blkno = GIN_ROOT_BLKNO;
	GinVacuumState gvs;
	Buffer		buffer;
	BlockNumber rootOfPostingTree[BLCKSZ / (sizeof(IndexTupleData) + sizeof(ItemId))];
	uint32		nRoot;

	gvs.tmpCxt = AllocSetContextCreate(CurrentMemoryContext,
									   "Gin vacuum temporary context",
									   ALLOCSET_DEFAULT_SIZES);
	gvs.index = index;
	gvs.callback = callback;
	gvs.callback_state = callback_state;
	gvs.strategy = info->strategy;
	initGinState(&gvs.ginstate, index);

	/* first time through? */
	if (stats == NULL)
	{
		/* Yes, so initialize stats to zeroes */
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

		/*
		 * and cleanup any pending inserts
		 */
		ginInsertCleanup(&gvs.ginstate, !AmAutoVacuumWorkerProcess(),
						 false, true, stats);
	}

	/* we'll re-count the tuples each time */
	stats->num_index_tuples = 0;
	gvs.result = stats;

	buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
								RBM_NORMAL, info->strategy);

	/* find leaf page */
	for (;;)
	{
		Page		page = BufferGetPage(buffer);
		IndexTuple	itup;

		LockBuffer(buffer, GIN_SHARE);

		Assert(!GinPageIsData(page));

		if (GinPageIsLeaf(page))
		{
			LockBuffer(buffer, GIN_UNLOCK);
			LockBuffer(buffer, GIN_EXCLUSIVE);

			if (blkno == GIN_ROOT_BLKNO && !GinPageIsLeaf(page))
			{
				LockBuffer(buffer, GIN_UNLOCK);
				continue;		/* check it one more */
			}
			break;
		}

		Assert(PageGetMaxOffsetNumber(page) >= FirstOffsetNumber);

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));
		blkno = GinGetDownlink(itup);
		Assert(blkno != InvalidBlockNumber);

		UnlockReleaseBuffer(buffer);
		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);
	}

	/* right now we found leftmost page in entry's BTree */

	for (;;)
	{
		Page		page = BufferGetPage(buffer);
		Page		resPage;
		uint32		i;

		Assert(!GinPageIsData(page));

		resPage = ginVacuumEntryPage(&gvs, buffer, rootOfPostingTree, &nRoot);

		blkno = GinPageGetOpaque(page)->rightlink;

		if (resPage)
		{
			START_CRIT_SECTION();
			PageRestoreTempPage(resPage, page);
			MarkBufferDirty(buffer);
			xlogVacuumPage(gvs.index, buffer);
			UnlockReleaseBuffer(buffer);
			END_CRIT_SECTION();
		}
		else
		{
			UnlockReleaseBuffer(buffer);
		}

		vacuum_delay_point();

		for (i = 0; i < nRoot; i++)
		{
			ginVacuumPostingTree(&gvs, rootOfPostingTree[i]);
			vacuum_delay_point();
		}

		if (blkno == InvalidBlockNumber)	/* rightmost page */
			break;

		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);
		LockBuffer(buffer, GIN_EXCLUSIVE);
	}

	MemoryContextDelete(gvs.tmpCxt);

	return gvs.result;
}

IndexBulkDeleteResult *
ginvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	index = info->index;
	bool		needLock;
	BlockNumber npages,
				blkno;
	BlockNumber totFreePages;
	GinState	ginstate;
	GinStatsData idxStat;

	/*
	 * In an autovacuum analyze, we want to clean up pending insertions.
	 * Otherwise, an ANALYZE-only call is a no-op.
	 */
	if (info->analyze_only)
	{
		if (AmAutoVacuumWorkerProcess())
		{
			initGinState(&ginstate, index);
			ginInsertCleanup(&ginstate, false, true, true, stats);
		}
		return stats;
	}

	/*
	 * Set up all-zero stats and cleanup pending inserts if ginbulkdelete
	 * wasn't called
	 */
	if (stats == NULL)
	{
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		initGinState(&ginstate, index);
		ginInsertCleanup(&ginstate, !AmAutoVacuumWorkerProcess(),
						 false, true, stats);
	}

	memset(&idxStat, 0, sizeof(idxStat));

	/*
	 * XXX we always report the heap tuple count as the number of index
	 * entries.  This is bogus if the index is partial, but it's real hard to
	 * tell how many distinct heap entries are referenced by a GIN index.
	 */
	stats->num_index_tuples = Max(info->num_heap_tuples, 0);
	stats->estimated_count = info->estimated_count;

	/*
	 * Need lock unless it's local to this backend.
	 */
	needLock = !RELATION_IS_LOCAL(index);

	if (needLock)
		LockRelationForExtension(index, ExclusiveLock);
	npages = RelationGetNumberOfBlocks(index);
	if (needLock)
		UnlockRelationForExtension(index, ExclusiveLock);

	totFreePages = 0;

	for (blkno = GIN_ROOT_BLKNO; blkno < npages; blkno++)
	{
		Buffer		buffer;
		Page		page;

		vacuum_delay_point();

		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);
		LockBuffer(buffer, GIN_SHARE);
		page = (Page) BufferGetPage(buffer);

		if (GinPageIsRecyclable(page))
		{
			Assert(blkno != GIN_ROOT_BLKNO);
			RecordFreeIndexPage(index, blkno);
			totFreePages++;
		}
		else if (GinPageIsData(page))
		{
			idxStat.nDataPages++;
		}
		else if (!GinPageIsList(page))
		{
			idxStat.nEntryPages++;

			if (GinPageIsLeaf(page))
				idxStat.nEntries += PageGetMaxOffsetNumber(page);
		}

		UnlockReleaseBuffer(buffer);
	}

	/* Update the metapage with accurate page and entry counts */
	idxStat.nTotalPages = npages;
	ginUpdateStats(info->index, &idxStat, false);

	/* Finally, vacuum the FSM */
	IndexFreeSpaceMapVacuum(info->index);

	stats->pages_free = totFreePages;

	if (needLock)
		LockRelationForExtension(index, ExclusiveLock);
	stats->num_pages = RelationGetNumberOfBlocks(index);
	if (needLock)
		UnlockRelationForExtension(index, ExclusiveLock);

	return stats;
}

/*
 * Return whether Page can safely be recycled.
 */
bool
GinPageIsRecyclable(Page page)
{
	TransactionId delete_xid;

	if (PageIsNew(page))
		return true;

	if (!GinPageIsDeleted(page))
		return false;

	delete_xid = GinPageGetDeleteXid(page);

	if (!TransactionIdIsValid(delete_xid))
		return true;

	/*
	 * If no backend still could view delete_xid as in running, all scans
	 * concurrent with ginDeletePage() must have finished.
	 */
	return GlobalVisCheckRemovableXid(NULL, delete_xid);
}
