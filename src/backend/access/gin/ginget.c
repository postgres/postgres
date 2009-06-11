/*-------------------------------------------------------------------------
 *
 * ginget.c
 *	  fetch tuples from a GIN scan.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gin/ginget.c,v 1.27 2009/06/11 14:48:53 momjian Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin.h"
#include "access/relscan.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/datum.h"
#include "utils/memutils.h"


typedef struct pendingPosition
{
	Buffer		pendingBuffer;
	OffsetNumber firstOffset;
	OffsetNumber lastOffset;
	ItemPointerData item;
} pendingPosition;


/*
 * Tries to refind previously taken ItemPointer on page.
 */
static bool
findItemInPage(Page page, ItemPointer item, OffsetNumber *off)
{
	OffsetNumber maxoff = GinPageGetOpaque(page)->maxoff;
	int			res;

	if (GinPageGetOpaque(page)->flags & GIN_DELETED)
		/* page was deleted by concurrent  vacuum */
		return false;

	/*
	 * scan page to find equal or first greater value
	 */

	for (*off = FirstOffsetNumber; *off <= maxoff; (*off)++)
	{
		res = compareItemPointers(item, (ItemPointer) GinDataPageGetItem(page, *off));

		if (res <= 0)
			return true;
	}

	return false;
}

/*
 * Goes to the next page if current offset is outside of bounds
 */
static bool
moveRightIfItNeeded(GinBtreeData *btree, GinBtreeStack *stack)
{
	Page		page = BufferGetPage(stack->buffer);

	if (stack->off > PageGetMaxOffsetNumber(page))
	{
		/*
		 * We scanned the whole page, so we should take right page
		 */
		stack->blkno = GinPageGetOpaque(page)->rightlink;

		if (GinPageRightMost(page))
			return false;		/* no more pages */

		LockBuffer(stack->buffer, GIN_UNLOCK);
		stack->buffer = ReleaseAndReadBuffer(stack->buffer, btree->index, stack->blkno);
		LockBuffer(stack->buffer, GIN_SHARE);
		stack->off = FirstOffsetNumber;
	}

	return true;
}

/*
 * Does fullscan of posting tree and saves ItemPointers
 * in scanEntry->partialMatch TIDBitmap
 */
static void
scanForItems(Relation index, GinScanEntry scanEntry, BlockNumber rootPostingTree)
{
	GinPostingTreeScan *gdi;
	Buffer		buffer;
	Page		page;
	BlockNumber blkno;

	gdi = prepareScanPostingTree(index, rootPostingTree, TRUE);

	buffer = scanBeginPostingTree(gdi);
	IncrBufferRefCount(buffer); /* prevent unpin in freeGinBtreeStack */

	freeGinBtreeStack(gdi->stack);
	pfree(gdi);

	/*
	 * Goes through all leaves
	 */
	for (;;)
	{
		page = BufferGetPage(buffer);

		if ((GinPageGetOpaque(page)->flags & GIN_DELETED) == 0 && GinPageGetOpaque(page)->maxoff >= FirstOffsetNumber)
		{
			tbm_add_tuples(scanEntry->partialMatch,
				   (ItemPointer) GinDataPageGetItem(page, FirstOffsetNumber),
						   GinPageGetOpaque(page)->maxoff, false);
			scanEntry->predictNumberResult += GinPageGetOpaque(page)->maxoff;
		}

		blkno = GinPageGetOpaque(page)->rightlink;
		if (GinPageRightMost(page))
		{
			UnlockReleaseBuffer(buffer);
			return;				/* no more pages */
		}

		LockBuffer(buffer, GIN_UNLOCK);
		buffer = ReleaseAndReadBuffer(buffer, index, blkno);
		LockBuffer(buffer, GIN_SHARE);
	}
}

/*
 * Collects all ItemPointer into the TIDBitmap struct
 * for entries partially matched to search entry.
 *
 * Returns true if done, false if it's needed to restart scan from scratch
 */
static bool
computePartialMatchList(GinBtreeData *btree, GinBtreeStack *stack, GinScanEntry scanEntry)
{
	Page		page;
	IndexTuple	itup;
	Datum		idatum;
	int32		cmp;

	scanEntry->partialMatch = tbm_create(work_mem * 1024L);

	for (;;)
	{
		/*
		 * stack->off points to the interested entry, buffer is already locked
		 */
		if (moveRightIfItNeeded(btree, stack) == false)
			return true;

		page = BufferGetPage(stack->buffer);
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stack->off));

		/*
		 * If tuple stores another attribute then stop scan
		 */
		if (gintuple_get_attrnum(btree->ginstate, itup) != scanEntry->attnum)
			return true;

		idatum = gin_index_getattr(btree->ginstate, itup);


		/*----------
		 * Check of partial match.
		 * case cmp == 0 => match
		 * case cmp > 0 => not match and finish scan
		 * case cmp < 0 => not match and continue scan
		 *----------
		 */
		cmp = DatumGetInt32(FunctionCall4(&btree->ginstate->comparePartialFn[scanEntry->attnum - 1],
										  scanEntry->entry,
										  idatum,
										  UInt16GetDatum(scanEntry->strategy),
									PointerGetDatum(scanEntry->extra_data)));

		if (cmp > 0)
			return true;
		else if (cmp < 0)
		{
			stack->off++;
			continue;
		}

		if (GinIsPostingTree(itup))
		{
			BlockNumber rootPostingTree = GinGetPostingTree(itup);
			Datum		newDatum,
						savedDatum = datumCopy(
											   idatum,
											   btree->ginstate->origTupdesc->attrs[scanEntry->attnum - 1]->attbyval,
			btree->ginstate->origTupdesc->attrs[scanEntry->attnum - 1]->attlen
			);

			/*
			 * We should unlock current page (but not unpin) during tree scan
			 * to prevent deadlock with vacuum processes.
			 *
			 * We save current entry value (savedDatum) to be able to refind
			 * our tuple after re-locking
			 */
			LockBuffer(stack->buffer, GIN_UNLOCK);
			scanForItems(btree->index, scanEntry, rootPostingTree);

			/*
			 * We lock again the entry page and while it was unlocked insert
			 * might occured, so we need to refind our position
			 */
			LockBuffer(stack->buffer, GIN_SHARE);
			page = BufferGetPage(stack->buffer);
			if (!GinPageIsLeaf(page))
			{
				/*
				 * Root page becomes non-leaf while we unlock it. We will
				 * start again, this situation doesn't cause often - root can
				 * became a non-leaf only one per life of index.
				 */

				return false;
			}

			for (;;)
			{
				if (moveRightIfItNeeded(btree, stack) == false)
					elog(ERROR, "lost saved point in index");	/* must not happen !!! */

				page = BufferGetPage(stack->buffer);
				itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stack->off));
				newDatum = gin_index_getattr(btree->ginstate, itup);

				if (gintuple_get_attrnum(btree->ginstate, itup) != scanEntry->attnum)
					elog(ERROR, "lost saved point in index");	/* must not happen !!! */

				if (compareEntries(btree->ginstate, scanEntry->attnum, newDatum, savedDatum) == 0)
				{
					/* Found!  */
					if (btree->ginstate->origTupdesc->attrs[scanEntry->attnum - 1]->attbyval == false)
						pfree(DatumGetPointer(savedDatum));
					break;
				}

				stack->off++;
			}
		}
		else
		{
			tbm_add_tuples(scanEntry->partialMatch, GinGetPosting(itup), GinGetNPosting(itup), false);
			scanEntry->predictNumberResult += GinGetNPosting(itup);
		}

		/*
		 * Ok, we save ItemPointers, go to the next entry
		 */
		stack->off++;
	}

	return true;
}

/*
 * Start* functions setup beginning state of searches: finds correct buffer and pins it.
 */
static void
startScanEntry(Relation index, GinState *ginstate, GinScanEntry entry)
{
	GinBtreeData btreeEntry;
	GinBtreeStack *stackEntry;
	Page		page;
	bool		needUnlock = TRUE;

	entry->buffer = InvalidBuffer;
	entry->offset = InvalidOffsetNumber;
	entry->list = NULL;
	entry->nlist = 0;
	entry->partialMatch = NULL;
	entry->partialMatchResult = NULL;
	entry->reduceResult = FALSE;
	entry->predictNumberResult = 0;

	if (entry->master != NULL)
	{
		entry->isFinished = entry->master->isFinished;
		return;
	}

	/*
	 * we should find entry, and begin scan of posting tree or just store
	 * posting list in memory
	 */

	prepareEntryScan(&btreeEntry, index, entry->attnum, entry->entry, ginstate);
	btreeEntry.searchMode = TRUE;
	stackEntry = ginFindLeafPage(&btreeEntry, NULL);
	page = BufferGetPage(stackEntry->buffer);

	entry->isFinished = TRUE;

	if (entry->isPartialMatch)
	{
		/*
		 * btreeEntry.findItem points to the first equal or greater value than
		 * needed. So we will scan further and collect all ItemPointers
		 */
		btreeEntry.findItem(&btreeEntry, stackEntry);
		if (computePartialMatchList(&btreeEntry, stackEntry, entry) == false)
		{
			/*
			 * GIN tree was seriously restructured, so we will cleanup all
			 * found data and rescan. See comments near 'return false' in
			 * computePartialMatchList()
			 */
			if (entry->partialMatch)
			{
				if (entry->partialMatchIterator)
					tbm_end_iterate(entry->partialMatchIterator);
				entry->partialMatchIterator = NULL;
				tbm_free(entry->partialMatch);
				entry->partialMatch = NULL;
			}
			LockBuffer(stackEntry->buffer, GIN_UNLOCK);
			freeGinBtreeStack(stackEntry);

			startScanEntry(index, ginstate, entry);
			return;
		}

		if (entry->partialMatch && !tbm_is_empty(entry->partialMatch))
		{
			entry->partialMatchIterator = tbm_begin_iterate(entry->partialMatch);
			entry->isFinished = FALSE;
		}
	}
	else if (btreeEntry.findItem(&btreeEntry, stackEntry))
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stackEntry->off));

		if (GinIsPostingTree(itup))
		{
			BlockNumber rootPostingTree = GinGetPostingTree(itup);
			GinPostingTreeScan *gdi;
			Page		page;

			/*
			 * We should unlock entry page before make deal with posting tree
			 * to prevent deadlocks with vacuum processes. Because entry is
			 * never deleted from page and posting tree is never reduced to
			 * the posting list, we can unlock page after getting BlockNumber
			 * of root of posting tree.
			 */
			LockBuffer(stackEntry->buffer, GIN_UNLOCK);
			needUnlock = FALSE;
			gdi = prepareScanPostingTree(index, rootPostingTree, TRUE);

			entry->buffer = scanBeginPostingTree(gdi);

			/*
			 * We keep buffer pinned because we need to prevent deletion of
			 * page during scan. See GIN's vacuum implementation. RefCount is
			 * increased to keep buffer pinned after freeGinBtreeStack() call.
			 */
			IncrBufferRefCount(entry->buffer);

			page = BufferGetPage(entry->buffer);
			entry->predictNumberResult = gdi->stack->predictNumber * GinPageGetOpaque(page)->maxoff;

			/*
			 * Keep page content in memory to prevent durable page locking
			 */
			entry->list = (ItemPointerData *) palloc(BLCKSZ);
			entry->nlist = GinPageGetOpaque(page)->maxoff;
			memcpy(entry->list, GinDataPageGetItem(page, FirstOffsetNumber),
				   GinPageGetOpaque(page)->maxoff * sizeof(ItemPointerData));

			LockBuffer(entry->buffer, GIN_UNLOCK);
			freeGinBtreeStack(gdi->stack);
			pfree(gdi);
			entry->isFinished = FALSE;
		}
		else if (GinGetNPosting(itup) > 0)
		{
			entry->nlist = GinGetNPosting(itup);
			entry->list = (ItemPointerData *) palloc(sizeof(ItemPointerData) * entry->nlist);
			memcpy(entry->list, GinGetPosting(itup), sizeof(ItemPointerData) * entry->nlist);
			entry->isFinished = FALSE;
		}
	}

	if (needUnlock)
		LockBuffer(stackEntry->buffer, GIN_UNLOCK);
	freeGinBtreeStack(stackEntry);
}

static void
startScanKey(Relation index, GinState *ginstate, GinScanKey key)
{
	uint32		i;

	if (!key->firstCall)
		return;

	for (i = 0; i < key->nentries; i++)
		startScanEntry(index, ginstate, key->scanEntry + i);

	memset(key->entryRes, TRUE, sizeof(bool) * key->nentries);
	key->isFinished = FALSE;
	key->firstCall = FALSE;

	if (GinFuzzySearchLimit > 0)
	{
		/*
		 * If all of keys more than threshold we will try to reduce result, we
		 * hope (and only hope, for intersection operation of array our
		 * supposition isn't true), that total result will not more than
		 * minimal predictNumberResult.
		 */

		for (i = 0; i < key->nentries; i++)
			if (key->scanEntry[i].predictNumberResult <= key->nentries * GinFuzzySearchLimit)
				return;

		for (i = 0; i < key->nentries; i++)
			if (key->scanEntry[i].predictNumberResult > key->nentries * GinFuzzySearchLimit)
			{
				key->scanEntry[i].predictNumberResult /= key->nentries;
				key->scanEntry[i].reduceResult = TRUE;
			}
	}
}

static void
startScan(IndexScanDesc scan)
{
	uint32		i;
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	for (i = 0; i < so->nkeys; i++)
		startScanKey(scan->indexRelation, &so->ginstate, so->keys + i);
}

/*
 * Gets next ItemPointer from PostingTree. Note, that we copy
 * page into GinScanEntry->list array and unlock page, but keep it pinned
 * to prevent interference with vacuum
 */
static void
entryGetNextItem(Relation index, GinScanEntry entry)
{
	Page		page;
	BlockNumber blkno;

	for (;;)
	{
		entry->offset++;

		if (entry->offset <= entry->nlist)
		{
			entry->curItem = entry->list[entry->offset - 1];
			return;
		}

		LockBuffer(entry->buffer, GIN_SHARE);
		page = BufferGetPage(entry->buffer);
		for (;;)
		{
			/*
			 * It's needed to go by right link. During that we should refind
			 * first ItemPointer greater that stored
			 */

			blkno = GinPageGetOpaque(page)->rightlink;

			LockBuffer(entry->buffer, GIN_UNLOCK);
			if (blkno == InvalidBlockNumber)
			{
				ReleaseBuffer(entry->buffer);
				ItemPointerSet(&entry->curItem, InvalidBlockNumber, InvalidOffsetNumber);
				entry->buffer = InvalidBuffer;
				entry->isFinished = TRUE;
				return;
			}

			entry->buffer = ReleaseAndReadBuffer(entry->buffer, index, blkno);
			LockBuffer(entry->buffer, GIN_SHARE);
			page = BufferGetPage(entry->buffer);

			entry->offset = InvalidOffsetNumber;
			if (!ItemPointerIsValid(&entry->curItem) || findItemInPage(page, &entry->curItem, &entry->offset))
			{
				/*
				 * Found position equal to or greater than stored
				 */
				entry->nlist = GinPageGetOpaque(page)->maxoff;
				memcpy(entry->list, GinDataPageGetItem(page, FirstOffsetNumber),
				   GinPageGetOpaque(page)->maxoff * sizeof(ItemPointerData));

				LockBuffer(entry->buffer, GIN_UNLOCK);

				if (!ItemPointerIsValid(&entry->curItem) ||
					compareItemPointers(&entry->curItem, entry->list + entry->offset - 1) == 0)
				{
					/*
					 * First pages are deleted or empty, or we found exact
					 * position, so break inner loop and continue outer one.
					 */

					break;
				}

				/*
				 * Find greater than entry->curItem position, store it.
				 */
				entry->curItem = entry->list[entry->offset - 1];

				return;
			}
		}
	}
}

#define gin_rand() (((double) random()) / ((double) MAX_RANDOM_VALUE))
#define dropItem(e) ( gin_rand() > ((double)GinFuzzySearchLimit)/((double)((e)->predictNumberResult)) )

/*
 * Sets entry->curItem to new found heap item pointer for one
 * entry of one scan key
 */
static bool
entryGetItem(Relation index, GinScanEntry entry)
{
	if (entry->master)
	{
		entry->isFinished = entry->master->isFinished;
		entry->curItem = entry->master->curItem;
	}
	else if (entry->partialMatch)
	{
		do
		{
			if (entry->partialMatchResult == NULL ||
				entry->offset >= entry->partialMatchResult->ntuples)
			{
				entry->partialMatchResult = tbm_iterate(entry->partialMatchIterator);

				if (entry->partialMatchResult == NULL)
				{
					ItemPointerSet(&entry->curItem, InvalidBlockNumber, InvalidOffsetNumber);
					tbm_end_iterate(entry->partialMatchIterator);
					entry->partialMatchIterator = NULL;
					entry->isFinished = TRUE;
					break;
				}

				/*
				 * reset counter to the beginning of
				 * entry->partialMatchResult. Note: entry->offset is still
				 * greater than partialMatchResult->ntuples if
				 * partialMatchResult is lossy. So, on next call we will get
				 * next result from TIDBitmap.
				 */
				entry->offset = 0;
			}

			if (entry->partialMatchResult->ntuples < 0)
			{
				/*
				 * lossy result, so we need to check the whole page
				 */
				ItemPointerSetLossyPage(&entry->curItem,
										entry->partialMatchResult->blockno);

				/*
				 * We might as well fall out of the loop; we could not
				 * estimate number of results on this page to support correct
				 * reducing of result even if it's enabled
				 */
				break;
			}

			ItemPointerSet(&entry->curItem,
						   entry->partialMatchResult->blockno,
						   entry->partialMatchResult->offsets[entry->offset]);
			entry->offset++;
		} while (entry->reduceResult == TRUE && dropItem(entry));
	}
	else if (!BufferIsValid(entry->buffer))
	{
		entry->offset++;
		if (entry->offset <= entry->nlist)
			entry->curItem = entry->list[entry->offset - 1];
		else
		{
			ItemPointerSet(&entry->curItem, InvalidBlockNumber, InvalidOffsetNumber);
			entry->isFinished = TRUE;
		}
	}
	else
	{
		do
		{
			entryGetNextItem(index, entry);
		} while (entry->isFinished == FALSE && entry->reduceResult == TRUE && dropItem(entry));
	}

	return entry->isFinished;
}

/*
 * Sets key->curItem to new found heap item pointer for one scan key
 * Returns isFinished, ie TRUE means we did NOT get a new item pointer!
 * Also, *keyrecheck is set true if recheck is needed for this scan key.
 * Note: lossy page could be returned after items from the same page.
 */
static bool
keyGetItem(Relation index, GinState *ginstate, MemoryContext tempCtx,
		   GinScanKey key, bool *keyrecheck)
{
	uint32		i;
	GinScanEntry entry;
	bool		res;
	MemoryContext oldCtx;

	if (key->isFinished)
		return TRUE;

	do
	{
		/*
		 * move forward from previously value and set new curItem, which is
		 * minimal from entries->curItems. Lossy page is encoded by
		 * ItemPointer with max value for offset (0xffff), so if there is an
		 * non-lossy entries on lossy page they will returned too and after
		 * that the whole page. That's not a problem for resulting tidbitmap.
		 */
		ItemPointerSetMax(&key->curItem);
		for (i = 0; i < key->nentries; i++)
		{
			entry = key->scanEntry + i;

			if (key->entryRes[i])
			{
				/*
				 * Move forward only entries which was the least on previous
				 * call, key->entryRes[i] points that current entry was a
				 * result of loop/call.
				 */
				if (entry->isFinished == FALSE && entryGetItem(index, entry) == FALSE)
				{
					if (compareItemPointers(&entry->curItem, &key->curItem) < 0)
						key->curItem = entry->curItem;
				}
				else
					key->entryRes[i] = FALSE;
			}
			else if (entry->isFinished == FALSE)
			{
				if (compareItemPointers(&entry->curItem, &key->curItem) < 0)
					key->curItem = entry->curItem;
			}
		}

		if (ItemPointerIsMax(&key->curItem))
		{
			/* all entries are finished */
			key->isFinished = TRUE;
			return TRUE;
		}

		/*
		 * Now key->curItem contains closest ItemPointer to previous result.
		 *
		 * if key->nentries == 1 then the consistentFn should always succeed,
		 * but we must call it anyway to find out the recheck status.
		 */

		/*----------
		 * entryRes array is used for:
		 * - as an argument for consistentFn
		 * - entry->curItem with corresponding key->entryRes[i] == false are
		 *	 greater than key->curItem, so next loop/call they should be
		 *	 renewed by entryGetItem(). So, we need to set up an array before
		 *	 checking of lossy page.
		 *----------
		 */
		for (i = 0; i < key->nentries; i++)
		{
			entry = key->scanEntry + i;

			if (entry->isFinished == FALSE &&
				compareItemPointers(&entry->curItem, &key->curItem) == 0)
				key->entryRes[i] = TRUE;
			else
				key->entryRes[i] = FALSE;
		}

		/*
		 * Initialize *keyrecheck in case the consistentFn doesn't know it
		 * should set it.  The safe assumption in that case is to force
		 * recheck.
		 */
		*keyrecheck = true;

		/*
		 * If one of the entry's scans returns lossy result, return it without
		 * further checking - we can't call consistentFn for lack of data.
		 */
		if (ItemPointerIsLossyPage(&key->curItem))
			return FALSE;

		oldCtx = MemoryContextSwitchTo(tempCtx);
		res = DatumGetBool(FunctionCall6(&ginstate->consistentFn[key->attnum - 1],
										 PointerGetDatum(key->entryRes),
										 UInt16GetDatum(key->strategy),
										 key->query,
										 UInt32GetDatum(key->nentries),
										 PointerGetDatum(key->extra_data),
										 PointerGetDatum(keyrecheck)));
		MemoryContextSwitchTo(oldCtx);
		MemoryContextReset(tempCtx);
	} while (!res);

	return FALSE;
}


/*
 * Get ItemPointer of next heap row to be checked from pending list.
 * Returns false if there are no more. On pages with several rows
 * it returns each row separately, on page with part of heap row returns
 * per page data.  pos->firstOffset and pos->lastOffset points
 * fraction of tuples for current heap row.
 *
 * The pendingBuffer is presumed pinned and share-locked on entry, and is
 * pinned and share-locked on success exit.  On failure exit it's released.
 */
static bool
scanGetCandidate(IndexScanDesc scan, pendingPosition *pos)
{
	OffsetNumber maxoff;
	Page		page;
	IndexTuple	itup;

	ItemPointerSetInvalid(&pos->item);
	for (;;)
	{
		page = BufferGetPage(pos->pendingBuffer);

		maxoff = PageGetMaxOffsetNumber(page);
		if (pos->firstOffset > maxoff)
		{
			BlockNumber blkno = GinPageGetOpaque(page)->rightlink;

			if (blkno == InvalidBlockNumber)
			{
				UnlockReleaseBuffer(pos->pendingBuffer);
				pos->pendingBuffer = InvalidBuffer;

				return false;
			}
			else
			{
				/*
				 * Here we must prevent deletion of next page by insertcleanup
				 * process, which may be trying to obtain exclusive lock on
				 * current page.  So, we lock next page before releasing the
				 * current one
				 */
				Buffer		tmpbuf = ReadBuffer(scan->indexRelation, blkno);

				LockBuffer(tmpbuf, GIN_SHARE);
				UnlockReleaseBuffer(pos->pendingBuffer);

				pos->pendingBuffer = tmpbuf;
				pos->firstOffset = FirstOffsetNumber;
			}
		}
		else
		{
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, pos->firstOffset));
			pos->item = itup->t_tid;
			if (GinPageHasFullRow(page))
			{
				/*
				 * find itempointer to the next row
				 */
				for (pos->lastOffset = pos->firstOffset + 1; pos->lastOffset <= maxoff; pos->lastOffset++)
				{
					itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, pos->lastOffset));
					if (!ItemPointerEquals(&pos->item, &itup->t_tid))
						break;
				}
			}
			else
			{
				/*
				 * All itempointers are the same on this page
				 */
				pos->lastOffset = maxoff + 1;
			}

			/*
			 * Now pos->firstOffset points to the first tuple of current heap
			 * row, pos->lastOffset points to the first tuple of second heap
			 * row (or to the end of page)
			 */

			break;
		}
	}

	return true;
}

/*
 * Scan page from current tuple (off) up till the first of:
 * - match is found (then returns true)
 * - no later match is possible
 * - tuple's attribute number is not equal to entry's attrnum
 * - reach end of page
 */
static bool
matchPartialInPendingList(GinState *ginstate, Page page,
						  OffsetNumber off, OffsetNumber maxoff,
						  Datum value, OffsetNumber attrnum,
						  Datum *datum, bool *datumExtracted,
						  StrategyNumber strategy,
						  Pointer extra_data)
{
	IndexTuple	itup;
	int32		cmp;

	while (off < maxoff)
	{
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));
		if (attrnum != gintuple_get_attrnum(ginstate, itup))
			return false;

		if (datumExtracted[off - 1] == false)
		{
			datum[off - 1] = gin_index_getattr(ginstate, itup);
			datumExtracted[off - 1] = true;
		}

		/*----------
		 * Check partial match.
		 * case cmp == 0 => match
		 * case cmp > 0 => not match and end scan (no later match possible)
		 * case cmp < 0 => not match and continue scan
		 *----------
		 */
		cmp = DatumGetInt32(FunctionCall4(&ginstate->comparePartialFn[attrnum - 1],
										  value,
										  datum[off - 1],
										  UInt16GetDatum(strategy),
										  PointerGetDatum(extra_data)));
		if (cmp == 0)
			return true;
		else if (cmp > 0)
			return false;

		off++;
	}

	return false;
}

/*
 * Sets entryRes array for each key by looking at
 * every entry per indexed value (heap's row) in pending list.
 * returns true if at least one of datum was matched by key's entry
 *
 * The pendingBuffer is presumed pinned and share-locked on entry.
 */
static bool
collectDatumForItem(IndexScanDesc scan, pendingPosition *pos)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	OffsetNumber attrnum;
	Page		page;
	IndexTuple	itup;
	int			i,
				j;
	bool		hasMatch = false;

	/*
	 * Resets entryRes
	 */
	for (i = 0; i < so->nkeys; i++)
	{
		GinScanKey	key = so->keys + i;

		memset(key->entryRes, FALSE, key->nentries);
	}

	for (;;)
	{
		Datum		datum[BLCKSZ / sizeof(IndexTupleData)];
		bool		datumExtracted[BLCKSZ / sizeof(IndexTupleData)];

		Assert(pos->lastOffset > pos->firstOffset);
		memset(datumExtracted + pos->firstOffset - 1, 0, sizeof(bool) * (pos->lastOffset - pos->firstOffset));

		page = BufferGetPage(pos->pendingBuffer);

		for (i = 0; i < so->nkeys; i++)
		{
			GinScanKey	key = so->keys + i;

			for (j = 0; j < key->nentries; j++)
			{
				OffsetNumber StopLow = pos->firstOffset,
							StopHigh = pos->lastOffset,
							StopMiddle;
				GinScanEntry entry = key->scanEntry + j;

				/* already true - do not extra work */
				if (key->entryRes[j])
					continue;

				/*
				 * Interested tuples are from pos->firstOffset to
				 * pos->lastOffset and they are ordered by (attnum, Datum) as
				 * it's done in entry tree So we could use binary search to
				 * prevent linear scanning
				 */
				while (StopLow < StopHigh)
				{
					StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);

					itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, StopMiddle));
					attrnum = gintuple_get_attrnum(&so->ginstate, itup);

					if (key->attnum < attrnum)
						StopHigh = StopMiddle;
					else if (key->attnum > attrnum)
						StopLow = StopMiddle + 1;
					else
					{
						int			res;

						if (datumExtracted[StopMiddle - 1] == false)
						{
							datum[StopMiddle - 1] = gin_index_getattr(&so->ginstate, itup);
							datumExtracted[StopMiddle - 1] = true;
						}
						res = compareEntries(&so->ginstate,
											 entry->attnum,
											 entry->entry,
											 datum[StopMiddle - 1]);

						if (res == 0)
						{
							/*
							 * The exact match causes, so we just scan from
							 * current position to find a partial match. See
							 * comment above about tuple's ordering.
							 */
							if (entry->isPartialMatch)
								key->entryRes[j] =
									matchPartialInPendingList(&so->ginstate,
															page, StopMiddle,
															  pos->lastOffset,
															  entry->entry,
															  entry->attnum,
															  datum,
															  datumExtracted,
															  entry->strategy,
														  entry->extra_data);
							else
								key->entryRes[j] = true;
							break;
						}
						else if (res < 0)
							StopHigh = StopMiddle;
						else
							StopLow = StopMiddle + 1;
					}
				}

				if (StopLow >= StopHigh && entry->isPartialMatch)
				{
					/*
					 * The exact match wasn't found, so we need to start scan
					 * from first tuple greater then current entry See comment
					 * above about tuple's ordering.
					 */
					key->entryRes[j] =
						matchPartialInPendingList(&so->ginstate,
												  page, StopHigh,
												  pos->lastOffset,
												  entry->entry,
												  entry->attnum,
												  datum,
												  datumExtracted,
												  entry->strategy,
												  entry->extra_data);
				}

				hasMatch |= key->entryRes[j];
			}
		}

		pos->firstOffset = pos->lastOffset;

		if (GinPageHasFullRow(page))
		{
			/*
			 * We scan all values from one tuple, go to next one
			 */

			return hasMatch;
		}
		else
		{
			ItemPointerData item = pos->item;

			/*
			 * need to get next portion of tuples of row containing on several
			 * pages
			 */

			if (scanGetCandidate(scan, pos) == false || !ItemPointerEquals(&pos->item, &item))
				elog(ERROR, "Could not process tuple"); /* XXX should not be
														 * here ! */
		}
	}

	return hasMatch;
}

/*
 * Collect all matched rows from pending list in bitmap
 */
static void
scanPendingInsert(IndexScanDesc scan, TIDBitmap *tbm, int64 *ntids)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	MemoryContext oldCtx;
	bool		recheck,
				keyrecheck,
				match;
	int			i;
	pendingPosition pos;
	Buffer		metabuffer = ReadBuffer(scan->indexRelation, GIN_METAPAGE_BLKNO);
	BlockNumber blkno;

	*ntids = 0;

	LockBuffer(metabuffer, GIN_SHARE);
	blkno = GinPageGetMeta(BufferGetPage(metabuffer))->head;

	/*
	 * fetch head of list before unlocking metapage. head page must be pinned
	 * to prevent deletion by vacuum process
	 */
	if (blkno == InvalidBlockNumber)
	{
		/* No pending list, so proceed with normal scan */
		UnlockReleaseBuffer(metabuffer);
		return;
	}

	pos.pendingBuffer = ReadBuffer(scan->indexRelation, blkno);
	LockBuffer(pos.pendingBuffer, GIN_SHARE);
	pos.firstOffset = FirstOffsetNumber;
	UnlockReleaseBuffer(metabuffer);

	/*
	 * loop for each heap row. scanGetCandidate returns full row or row's
	 * tuples from first page.
	 */
	while (scanGetCandidate(scan, &pos))
	{
		/*
		 * Check entries in tuple and setup entryRes array If tuples of heap's
		 * row are placed on several pages collectDatumForItem will read all
		 * of that pages.
		 */
		if (!collectDatumForItem(scan, &pos))
			continue;

		/*
		 * Matching of entries of one row is finished, so check row by
		 * consistent function.
		 */
		oldCtx = MemoryContextSwitchTo(so->tempCtx);
		recheck = false;
		match = true;

		for (i = 0; i < so->nkeys; i++)
		{
			GinScanKey	key = so->keys + i;

			keyrecheck = true;

			if (!DatumGetBool(FunctionCall6(&so->ginstate.consistentFn[key->attnum - 1],
											PointerGetDatum(key->entryRes),
											UInt16GetDatum(key->strategy),
											key->query,
											UInt32GetDatum(key->nentries),
											PointerGetDatum(key->extra_data),
											PointerGetDatum(&keyrecheck))))
			{
				match = false;
				break;
			}

			recheck |= keyrecheck;
		}

		MemoryContextSwitchTo(oldCtx);
		MemoryContextReset(so->tempCtx);

		if (match)
		{
			tbm_add_tuples(tbm, &pos.item, 1, recheck);
			(*ntids)++;
		}
	}
}

/*
 * Get heap item pointer from scan
 * returns true if found
 */
static bool
scanGetItem(IndexScanDesc scan, ItemPointerData *item, bool *recheck)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	uint32		i;
	bool		keyrecheck;

	/*
	 * We return recheck = true if any of the keyGetItem calls return
	 * keyrecheck = true.  Note that because the second loop might advance
	 * some keys, this could theoretically be too conservative.  In practice
	 * though, we expect that a consistentFn's recheck result will depend only
	 * on the operator and the query, so for any one key it should stay the
	 * same regardless of advancing to new items.  So it's not worth working
	 * harder.
	 */
	*recheck = false;

	ItemPointerSetMin(item);
	for (i = 0; i < so->nkeys; i++)
	{
		GinScanKey	key = so->keys + i;

		if (keyGetItem(scan->indexRelation, &so->ginstate, so->tempCtx,
					   key, &keyrecheck))
			return FALSE;		/* finished one of keys */
		if (compareItemPointers(item, &key->curItem) < 0)
			*item = key->curItem;
		*recheck |= keyrecheck;
	}

	for (i = 1; i <= so->nkeys; i++)
	{
		GinScanKey	key = so->keys + i - 1;

		for (;;)
		{
			int			cmp = compareItemPointers(item, &key->curItem);

			if (cmp != 0 && (ItemPointerIsLossyPage(item) || ItemPointerIsLossyPage(&key->curItem)))
			{
				/*
				 * if one of ItemPointers points to the whole page then
				 * compare only page's number
				 */
				if (ItemPointerGetBlockNumber(item) == ItemPointerGetBlockNumber(&key->curItem))
					cmp = 0;
				else
					cmp = (ItemPointerGetBlockNumber(item) > ItemPointerGetBlockNumber(&key->curItem)) ? 1 : -1;
			}

			if (cmp == 0)
				break;
			else if (cmp > 0)
			{
				if (keyGetItem(scan->indexRelation, &so->ginstate, so->tempCtx,
							   key, &keyrecheck))
					return FALSE;		/* finished one of keys */
				*recheck |= keyrecheck;
			}
			else
			{					/* returns to begin */
				*item = key->curItem;
				i = 0;
				break;
			}
		}
	}

	return TRUE;
}

#define GinIsNewKey(s)		( ((GinScanOpaque) scan->opaque)->keys == NULL )
#define GinIsVoidRes(s)		( ((GinScanOpaque) scan->opaque)->isVoidRes == true )

Datum
gingetbitmap(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	TIDBitmap  *tbm = (TIDBitmap *) PG_GETARG_POINTER(1);
	int64		ntids;

	if (GinIsNewKey(scan))
		newScanKey(scan);

	if (GinIsVoidRes(scan))
		PG_RETURN_INT64(0);

	ntids = 0;

	/*
	 * First, scan the pending list and collect any matching entries into the
	 * bitmap.	After we scan a pending item, some other backend could post it
	 * into the main index, and so we might visit it a second time during the
	 * main scan.  This is okay because we'll just re-set the same bit in the
	 * bitmap.	(The possibility of duplicate visits is a major reason why GIN
	 * can't support the amgettuple API, however.) Note that it would not do
	 * to scan the main index before the pending list, since concurrent
	 * cleanup could then make us miss entries entirely.
	 */
	scanPendingInsert(scan, tbm, &ntids);

	/*
	 * Now scan the main index.
	 */
	startScan(scan);

	for (;;)
	{
		ItemPointerData iptr;
		bool		recheck;

		CHECK_FOR_INTERRUPTS();

		if (!scanGetItem(scan, &iptr, &recheck))
			break;

		if (ItemPointerIsLossyPage(&iptr))
			tbm_add_page(tbm, ItemPointerGetBlockNumber(&iptr));
		else
			tbm_add_tuples(tbm, &iptr, 1, recheck);
		ntids++;
	}

	PG_RETURN_INT64(ntids);
}
