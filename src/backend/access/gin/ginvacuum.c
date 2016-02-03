/*-------------------------------------------------------------------------
 *
 * ginvacuum.c
 *	  delete & vacuum routines for the postgres GIN
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginvacuum.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/xloginsert.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
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

static bool
ginVacuumPostingTreeLeaves(GinVacuumState *gvs, BlockNumber blkno, bool isRoot, Buffer *rootBuffer)
{
	Buffer		buffer;
	Page		page;
	bool		hasVoidPage = FALSE;
	MemoryContext oldCxt;

	buffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, blkno,
								RBM_NORMAL, gvs->strategy);
	page = BufferGetPage(buffer);

	/*
	 * We should be sure that we don't concurrent with inserts, insert process
	 * never release root page until end (but it can unlock it and lock
	 * again). New scan can't start but previously started ones work
	 * concurrently.
	 */
	if (isRoot)
		LockBufferForCleanup(buffer);
	else
		LockBuffer(buffer, GIN_EXCLUSIVE);

	Assert(GinPageIsData(page));

	if (GinPageIsLeaf(page))
	{
		oldCxt = MemoryContextSwitchTo(gvs->tmpCxt);
		ginVacuumPostingTreeLeaf(gvs->index, buffer, gvs);
		MemoryContextSwitchTo(oldCxt);
		MemoryContextReset(gvs->tmpCxt);

		/* if root is a leaf page, we don't desire further processing */
		if (!isRoot && !hasVoidPage && GinDataLeafPageIsEmpty(page))
			hasVoidPage = TRUE;
	}
	else
	{
		OffsetNumber i;
		bool		isChildHasVoid = FALSE;

		for (i = FirstOffsetNumber; i <= GinPageGetOpaque(page)->maxoff; i++)
		{
			PostingItem *pitem = GinDataPageGetPostingItem(page, i);

			if (ginVacuumPostingTreeLeaves(gvs, PostingItemGetBlockNumber(pitem), FALSE, NULL))
				isChildHasVoid = TRUE;
		}

		if (isChildHasVoid)
			hasVoidPage = TRUE;
	}

	/*
	 * if we have root and there are empty pages in tree, then we don't
	 * release lock to go further processing and guarantee that tree is unused
	 */
	if (!(isRoot && hasVoidPage))
	{
		UnlockReleaseBuffer(buffer);
	}
	else
	{
		Assert(rootBuffer);
		*rootBuffer = buffer;
	}

	return hasVoidPage;
}

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
	 * Lock the pages in the same order as an insertion would, to avoid
	 * deadlocks: left, then right, then parent.
	 */
	lBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, leftBlkno,
								 RBM_NORMAL, gvs->strategy);
	dBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, deleteBlkno,
								 RBM_NORMAL, gvs->strategy);
	pBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, parentBlkno,
								 RBM_NORMAL, gvs->strategy);

	LockBuffer(lBuffer, GIN_EXCLUSIVE);
	LockBuffer(dBuffer, GIN_EXCLUSIVE);
	if (!isParentRoot)			/* parent is already locked by
								 * LockBufferForCleanup() */
		LockBuffer(pBuffer, GIN_EXCLUSIVE);

	START_CRIT_SECTION();

	/* Unlink the page by changing left sibling's rightlink */
	page = BufferGetPage(dBuffer);
	rightlink = GinPageGetOpaque(page)->rightlink;

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
	GinPageGetOpaque(page)->flags = GIN_DELETED;

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

		XLogRegisterData((char *) &data, sizeof(ginxlogDeletePage));

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_DELETE_PAGE);
		PageSetLSN(page, recptr);
		PageSetLSN(parentPage, recptr);
		PageSetLSN(BufferGetPage(lBuffer), recptr);
	}

	if (!isParentRoot)
		LockBuffer(pBuffer, GIN_UNLOCK);
	ReleaseBuffer(pBuffer);
	UnlockReleaseBuffer(lBuffer);
	UnlockReleaseBuffer(dBuffer);

	END_CRIT_SECTION();

	gvs->result->pages_deleted++;
}

typedef struct DataPageDeleteStack
{
	struct DataPageDeleteStack *child;
	struct DataPageDeleteStack *parent;

	BlockNumber blkno;			/* current block number */
	BlockNumber leftBlkno;		/* rightest non-deleted page on left */
	bool		isRoot;
} DataPageDeleteStack;

/*
 * scans posting tree and deletes empty pages
 */
static bool
ginScanToDelete(GinVacuumState *gvs, BlockNumber blkno, bool isRoot,
				DataPageDeleteStack *parent, OffsetNumber myoff)
{
	DataPageDeleteStack *me;
	Buffer		buffer;
	Page		page;
	bool		meDelete = FALSE;
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
			me->leftBlkno = InvalidBlockNumber;
		}
		else
			me = parent->child;
	}

	buffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, blkno,
								RBM_NORMAL, gvs->strategy);
	page = BufferGetPage(buffer);

	Assert(GinPageIsData(page));

	if (!GinPageIsLeaf(page))
	{
		OffsetNumber i;

		me->blkno = blkno;
		for (i = FirstOffsetNumber; i <= GinPageGetOpaque(page)->maxoff; i++)
		{
			PostingItem *pitem = GinDataPageGetPostingItem(page, i);

			if (ginScanToDelete(gvs, PostingItemGetBlockNumber(pitem), FALSE, me, i))
				i--;
		}
	}

	if (GinPageIsLeaf(page))
		isempty = GinDataLeafPageIsEmpty(page);
	else
		isempty = GinPageGetOpaque(page)->maxoff < FirstOffsetNumber;

	if (isempty)
	{
		/* we never delete the left- or rightmost branch */
		if (me->leftBlkno != InvalidBlockNumber && !GinPageRightMost(page))
		{
			Assert(!isRoot);
			ginDeletePage(gvs, blkno, me->leftBlkno, me->parent->blkno, myoff, me->parent->isRoot);
			meDelete = TRUE;
		}
	}

	ReleaseBuffer(buffer);

	if (!meDelete)
		me->leftBlkno = blkno;

	return meDelete;
}

static void
ginVacuumPostingTree(GinVacuumState *gvs, BlockNumber rootBlkno)
{
	Buffer		rootBuffer = InvalidBuffer;
	DataPageDeleteStack root,
			   *ptr,
			   *tmp;

	if (ginVacuumPostingTreeLeaves(gvs, rootBlkno, TRUE, &rootBuffer) == FALSE)
	{
		Assert(rootBuffer == InvalidBuffer);
		return;
	}

	memset(&root, 0, sizeof(DataPageDeleteStack));
	root.leftBlkno = InvalidBlockNumber;
	root.isRoot = TRUE;

	vacuum_delay_point();

	ginScanToDelete(gvs, rootBlkno, TRUE, &root, InvalidOffsetNumber);

	ptr = root.child;
	while (ptr)
	{
		tmp = ptr->child;
		pfree(ptr);
		ptr = tmp;
	}

	UnlockReleaseBuffer(rootBuffer);
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
									   ALLOCSET_DEFAULT_MINSIZE,
									   ALLOCSET_DEFAULT_INITSIZE,
									   ALLOCSET_DEFAULT_MAXSIZE);
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
		/* and cleanup any pending inserts */
		ginInsertCleanup(&gvs.ginstate, false, stats);
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

		if (blkno == InvalidBlockNumber)		/* rightmost page */
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
		if (IsAutoVacuumWorkerProcess())
		{
			initGinState(&ginstate, index);
			ginInsertCleanup(&ginstate, true, stats);
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
		ginInsertCleanup(&ginstate, false, stats);
	}

	memset(&idxStat, 0, sizeof(idxStat));

	/*
	 * XXX we always report the heap tuple count as the number of index
	 * entries.  This is bogus if the index is partial, but it's real hard to
	 * tell how many distinct heap entries are referenced by a GIN index.
	 */
	stats->num_index_tuples = info->num_heap_tuples;
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

		if (PageIsNew(page) || GinPageIsDeleted(page))
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
	ginUpdateStats(info->index, &idxStat);

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
